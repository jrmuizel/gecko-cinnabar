#![allow(improper_ctypes)] // this is needed so that rustc doesn't complain about passing the &Arc<Vec> to an extern function
use webrender::api::*;
use bindings::{ByteSlice, MutByteSlice, wr_moz2d_render_cb, ArcVecU8};
use rayon::ThreadPool;

use std::collections::hash_map::{HashMap, Entry};
use std::mem;
use std::os::raw::c_void;
use std::ptr;
use std::sync::mpsc::{channel, Sender, Receiver};
use std::sync::Arc;

#[cfg(target_os = "windows")]
use dwrote;

#[cfg(target_os = "macos")]
use foreign_types::ForeignType;

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
use std::ffi::CString;

pub struct Moz2dImageRenderer {
    blob_commands: HashMap<ImageKey, (Arc<BlobImageData>, Option<TileSize>)>,

    // The images rendered in the current frame (not kept here between frames)
    rendered_images: HashMap<BlobImageRequest, Option<BlobImageResult>>,

    tx: Sender<(BlobImageRequest, BlobImageResult)>,
    rx: Receiver<(BlobImageRequest, BlobImageResult)>,

    workers: Arc<ThreadPool>,
}

fn option_to_nullable<T>(option: &Option<T>) -> *const T {
    match option {
        &Some(ref x) => { x as *const T }
        &None => { ptr::null() }
    }
}

fn to_usize(slice: &[u8]) -> usize {
    convert_from_bytes(slice)
}

fn convert_from_bytes<T>(slice: &[u8]) -> T {
    assert!(mem::size_of::<T>() <= slice.len());
    let mut ret: T;
    unsafe {
        ret = mem::uninitialized();
        ptr::copy_nonoverlapping(slice.as_ptr(),
                                 &mut ret as *mut T as *mut u8,
                                 mem::size_of::<T>());
    }
    ret
}

use std::slice;

fn convert_to_bytes<T>(x: &T) -> &[u8] {
    unsafe {
        let ip: *const T = x;
        let bp: *const u8 = ip as * const _;
        slice::from_raw_parts(bp, mem::size_of::<T>())
    }
}


struct BufReader<'a>
{
    buf: &'a[u8],
    pos: usize,
}

impl<'a> BufReader<'a> {
    fn new(buf: &'a[u8]) -> BufReader<'a> {
        BufReader{ buf: buf, pos: 0 }
    }

    fn read<T>(&mut self) -> T {
        let ret = convert_from_bytes(&self.buf[self.pos..]);
        self.pos += mem::size_of::<T>();
        ret
    }

    fn read_font_key(&mut self) -> FontKey {
        self.read()
    }

    fn read_usize(&mut self) -> usize {
        self.read()
    }

    fn read_entry(&mut self) -> (usize, usize, Box2d) {
        let end = self.read();
        let extra_end = self.read();
        let bounds = self.read();
        (end, extra_end, bounds)
    }
}

// This is used for writing new blob images.
// In our case this is the result of merging an old one and a new one
struct BufWriter
{
    buf: Vec<u8>,
    index: Vec<u8>
}


impl BufWriter {
    fn new() -> BufWriter {
        BufWriter{ buf: Vec::new(), index: Vec::new() }
    }

    fn new_entry(&mut self, extra_size: usize, bounds: Box2d, data: &[u8]) {
        self.buf.extend_from_slice(data);
        self.index.extend_from_slice(convert_to_bytes(&(self.buf.len() - extra_size)));
        self.index.extend_from_slice(convert_to_bytes(&self.buf.len()));
        // XXX: we can agregate these writes
        self.index.extend_from_slice(convert_to_bytes(&bounds.x1));
        self.index.extend_from_slice(convert_to_bytes(&bounds.y1));
        self.index.extend_from_slice(convert_to_bytes(&bounds.x2));
        self.index.extend_from_slice(convert_to_bytes(&bounds.y2));
    }

    fn finish(mut self) -> Vec<u8> {
        // append the index to the end of the buffer
        // and then append the offset to the begining of the index
        let index_begin = self.buf.len();
        self.buf.extend_from_slice(&self.index);
        self.buf.extend_from_slice(convert_to_bytes(&index_begin));
        self.buf
    }
}


// XXX: Do we want to allow negative values here or clamp to the image bounds?
#[derive(Debug, Eq, PartialEq)]
struct Box2d {
    x1: u32,
    y1: u32,
    x2: u32,
    y2: u32
}

impl Box2d {
    fn contained_by(&self, other: &Box2d) -> bool {
        self.x1 >= other.x1 &&
        self.x2 <= other.x2 &&
        self.y1 >= other.y1 &&
        self.y2 <= other.y2
    }
    fn intersects(&self, other: &Box2d) -> bool {
        self.x1 < other.x2 &&
        self.x2 > other.x1 &&
        self.y1 < other.y2 &&
        self.y2 > other.y1
    }
    fn overlaps(&self, other: &Box2d) -> bool {
        self.intersects(other) && !self.contained_by(other)
    }
}

fn create_index_reader(buf: &[u8]) -> BufReader {
    // the offset of the index is at the end of the buffer
    let index_offset_pos = buf.len()-mem::size_of::<usize>();
    let index_offset = to_usize(&buf[index_offset_pos..]);

    BufReader::new(&buf[index_offset..index_offset_pos])
}

/* The invarients that we need for this to work properly are that
 * - all new content is contained in the dirty_rect
 * - all content that overlaps with the dirty_rect is included in the new index
 *   this is needed so that we can properly synchronize our buffers
 */
fn merge_blob_images(old: &[u8], new: &[u8], dirty_rect: DeviceUintRect, ) -> Arc<Vec<u8>> {

    let dirty_rect = Box2d{ x1: dirty_rect.min_x(), y1: dirty_rect.min_y(), x2: dirty_rect.max_x(), y2: dirty_rect.max_y() };

    let mut result = BufWriter::new();

    let mut index = create_index_reader(old);
    let mut new_index = create_index_reader(new);

    // loop over both new and old entries merging them
    // both new and old must have the same number of entries that
    // overlap but are not contained by the dirty rect
    let mut begin = 0;
    let mut new_begin = 0;
    println!("dirty rect: {:?}", dirty_rect);
    while index.pos < index.buf.len() {
        let (extra, end, bounds) = index.read_entry();
        println!("bounds: {} {} {:?}", extra, end, bounds);
        if bounds.contained_by(&dirty_rect) {
            println!("skip");
            // skip these items as they will be replaced with items from new
        } else if bounds.overlaps(&dirty_rect) {
            // this is a sync point between the old and new lists
            // find matching rect in new list.
            while new_index.pos < new_index.buf.len() {
                let (new_extra, new_end, new_bounds) = new_index.read_entry();
                println!("new bounds: {} {} {:?}", new_extra, new_end, new_bounds);

                if new_bounds.contained_by(&dirty_rect) {
                    println!("new item");
                    
                    result.new_entry(new_end - new_extra, new_bounds, &new[new_begin..new_end]);
                } else if new_bounds.overlaps(&dirty_rect) {
                    // you might thing that bounds == new_bounds, but sequence points might not be in the same
                    // because of an earlier update. We don't need them to be in the same order
                    // we just need the same number of them.
                    result.new_entry(new_end - new_extra, new_bounds, &new[new_begin..new_end]);
                    new_begin = new_end;
                    break;
                } else {
                    panic!("new bounds outside of dirty rect {:?} {:?}", new_bounds, dirty_rect);
                }
                new_begin = new_end;
            }
        } else {
            result.new_entry(end - extra, bounds, &old[begin..end]);
        }
        begin = end;
    }
    // include any remaining old items
    while new_index.pos < new_index.buf.len() {
        let (new_extra, new_end, new_bounds) = new_index.read_entry();
        println!("new bounds: {} {} {:?}", new_extra, new_end, new_bounds);
        if new_bounds.contained_by(&dirty_rect) {
            result.new_entry(new_end - new_extra, new_bounds, &new[new_begin..new_end]);
        } else {
            panic!("only fully contained items should be left: {:?} vs {:?}", new_bounds, dirty_rect);
        }
        new_begin = new_end;
    }

    let k = result.finish();
    {
        let mut index = create_index_reader(&k);
        assert!(index.pos < index.buf.len(), "Unexpectedly empty result. This blob should just have been deleted");
        while index.pos < index.buf.len() {
            let (extra, end, bounds) = index.read_entry();
            println!("result bounds: {} {} {:?}", extra, end, bounds);
        }
    }

    Arc::new(k)
}

impl BlobImageRenderer for Moz2dImageRenderer {
    fn add(&mut self, key: ImageKey, data: BlobImageData, tiling: Option<TileSize>) {
        self.blob_commands.insert(key, (Arc::new(data), tiling));
    }

    fn update(&mut self, key: ImageKey, data: BlobImageData, dirty_rect: Option<DeviceUintRect>) {
        match self.blob_commands.entry(key) {
            Entry::Occupied(mut e) => {
                let old_data = &mut e.get_mut().0;
                *old_data = merge_blob_images(&old_data, &data, dirty_rect.unwrap());
            }
            _ => { panic!("missing image key"); }
        }
    }

    fn delete(&mut self, key: ImageKey) {
        self.blob_commands.remove(&key);
    }

    fn request(&mut self,
               resources: &BlobImageResources,
               request: BlobImageRequest,
               descriptor: &BlobImageDescriptor,
               _dirty_rect: Option<DeviceUintRect>) {
        debug_assert!(!self.rendered_images.contains_key(&request), "{:?}", request);
        // TODO: implement tiling.

        // Add None in the map of rendered images. This makes it possible to differentiate
        // between commands that aren't finished yet (entry in the map is equal to None) and
        // keys that have never been requested (entry not in the map), which would cause deadlocks
        // if we were to block upon receving their result in resolve!
        self.rendered_images.insert(request, None);

        let tx = self.tx.clone();
        let descriptor = descriptor.clone();
        let blob = &self.blob_commands[&request.key];
        let tile_size = blob.1;
        let commands = Arc::clone(&blob.0);

        #[cfg(target_os = "windows")]
        fn process_native_font_handle(key: FontKey, handle: &NativeFontHandle) {
            let system_fc = dwrote::FontCollection::system();
            let font = system_fc.get_font_from_descriptor(handle).unwrap();
            let face = font.create_font_face();
            unsafe { AddNativeFontHandle(key, face.as_ptr() as *mut c_void, 0) };
        }

        #[cfg(target_os = "macos")]
        fn process_native_font_handle(key: FontKey, handle: &NativeFontHandle) {
            unsafe { AddNativeFontHandle(key, handle.0.as_ptr() as *mut c_void, 0) };
        }

        #[cfg(not(any(target_os = "macos", target_os = "windows")))]
        fn process_native_font_handle(key: FontKey, handle: &NativeFontHandle) {
            let cstr = CString::new(handle.pathname.clone()).unwrap();
            unsafe { AddNativeFontHandle(key, cstr.as_ptr() as *mut c_void, handle.index) };
        }

        fn process_fonts(mut extra_data: BufReader, resources: &BlobImageResources) {
            let font_count = extra_data.read_usize();
            for _ in 0..font_count {
                let key = extra_data.read_font_key();
                let template = resources.get_font_data(key);
                match template {
                    &FontTemplate::Raw(ref data, ref index) => {
                        unsafe { AddFontData(key, data.as_ptr(), data.len(), *index, data); }
                    }
                    &FontTemplate::Native(ref handle) => {
                        process_native_font_handle(key, handle);
                    }
                }
                resources.get_font_data(key);
            }
        }
        {
            let mut index = create_index_reader(&commands);
            while index.pos < index.buf.len() {
                let (end, extra_end, _)  = index.read_entry();
                process_fonts(BufReader::new(&commands[end..extra_end]), resources);
            }
        }

        self.workers.spawn(move || {
            let buf_size = (descriptor.width
                * descriptor.height
                * descriptor.format.bytes_per_pixel()) as usize;
            let mut output = vec![0u8; buf_size];

            let result = unsafe {
                if wr_moz2d_render_cb(
                    ByteSlice::new(&commands[..]),
                    descriptor.width,
                    descriptor.height,
                    descriptor.format,
                    option_to_nullable(&tile_size),
                    option_to_nullable(&request.tile),
                    MutByteSlice::new(output.as_mut_slice())
                ) {

                    Ok(RasterizedBlobImage {
                        width: descriptor.width,
                        height: descriptor.height,
                        data: output,
                    })
                } else {
                    Err(BlobImageError::Other("Unknown error".to_string()))
                }
            };

            tx.send((request, result)).unwrap();
        });
    }

    fn resolve(&mut self, request: BlobImageRequest) -> BlobImageResult {

        match self.rendered_images.entry(request) {
            Entry::Vacant(_) => {
                return Err(BlobImageError::InvalidKey);
            }
            Entry::Occupied(entry) => {
                // None means we haven't yet received the result.
                if entry.get().is_some() {
                    let result = entry.remove();
                    return result.unwrap();
                }
            }
        }

        // We haven't received it yet, pull from the channel until we receive it.
        while let Ok((req, result)) = self.rx.recv() {
            if req == request {
                // There it is!
                self.rendered_images.remove(&request);
                return result
            }
            self.rendered_images.insert(req, Some(result));
        }

        // If we break out of the loop above it means the channel closed unexpectedly.
        Err(BlobImageError::Other("Channel closed".into()))
    }
    fn delete_font(&mut self, font: FontKey) {
        unsafe { DeleteFontData(font); }
    }

    fn delete_font_instance(&mut self, _key: FontInstanceKey) {
    }
}

use bindings::WrFontKey;
extern "C" {
    #[allow(improper_ctypes)]
    fn AddFontData(key: WrFontKey, data: *const u8, size: usize, index: u32, vec: &ArcVecU8);
    fn AddNativeFontHandle(key: WrFontKey, handle: *mut c_void, index: u32);
    fn DeleteFontData(key: WrFontKey);
}

impl Moz2dImageRenderer {
    pub fn new(workers: Arc<ThreadPool>) -> Self {
        let (tx, rx) = channel();
        Moz2dImageRenderer {
            blob_commands: HashMap::new(),
            rendered_images: HashMap::new(),
            workers: workers,
            tx: tx,
            rx: rx,
        }
    }
}


#[cfg(test)]
mod tests {
    use moz2d_renderer::*;
    #[test]
    fn test_basic_merging() {
        let mut buf1 = BufWriter::new();
        buf1.new_entry(0, Box2d { x1: 0, y1: 0, x2: 1, y2: 1 }, &[1, 2, 3, 4]);
        let buf1 = buf1.finish();
        let buf2 = BufWriter::new();
        let buf2 = buf2.finish();
        let dirty_rect = DeviceUintRect::new(
            DeviceUintPoint::new(10, 10),
            DeviceUintSize::new(100, 100));
        merge_blob_images(&buf1, &buf2, dirty_rect);
    }
    #[test]
    fn test_deleting() {
        let mut buf1 = BufWriter::new();
        buf1.new_entry(0, Box2d { x1: 0, y1: 0, x2: 1, y2: 1 }, &[1, 2, 3, 4]);
        buf1.new_entry(0, Box2d { x1: 20, y1: 20, x2: 21, y2: 21 }, &[1, 2, 3, 4]);
        let buf1 = buf1.finish();
        let buf2 = BufWriter::new();
        let buf2 = buf2.finish();
        let dirty_rect = DeviceUintRect::new(
            DeviceUintPoint::new(10, 10),
            DeviceUintSize::new(100, 100));
        merge_blob_images(&buf1, &buf2, dirty_rect);
    }
    #[test]
    fn test_ordering() {
        // test that the merge algorithm works even though sequence points might get reordered
        let mut buf1 = BufWriter::new();
        buf1.new_entry(0, Box2d { x1: 0, y1: 0, x2: 1, y2: 1 }, &[1, 2, 3, 4]);
        buf1.new_entry(0, Box2d { x1: 30, y1: 10, x2: 31, y2: 11 }, &[1, 2, 3, 4]);
        buf1.new_entry(0, Box2d { x1: 20, y1: 20, x2: 51, y2: 21 }, &[1, 2, 3, 4]);
        buf1.new_entry(0, Box2d { x1: 20, y1: 30, x2: 51, y2: 31 }, &[1, 2, 3, 4]);
        buf1.new_entry(0, Box2d { x1: 30, y1: 40, x2: 31, y2: 41 }, &[1, 2, 3, 4]);
        let buf1 = buf1.finish();
        let mut buf2 = BufWriter::new();
        buf2.new_entry(0, Box2d { x1: 20, y1: 20, x2: 51, y2: 21 }, &[1, 2, 3, 4]);
        let buf2 = buf2.finish();
        let dirty_rect = DeviceUintRect::new(
            DeviceUintPoint::new(20, 20),
            DeviceUintSize::new(31, 1));
        let result = merge_blob_images(&buf1, &buf2, dirty_rect);

        let mut buf2 = BufWriter::new();
        buf2.new_entry(0, Box2d { x1: 30, y1: 10, x2: 31, y2: 11 }, &[1, 2, 3, 4]);
        buf2.new_entry(0, Box2d { x1: 20, y1: 20, x2: 51, y2: 21 }, &[1, 2, 3, 4]);
        buf2.new_entry(0, Box2d { x1: 20, y1: 30, x2: 51, y2: 31 }, &[1, 2, 3, 4]);
        buf2.new_entry(0, Box2d { x1: 30, y1: 40, x2: 31, y2: 41 }, &[1, 2, 3, 4]);
        let buf2 = buf2.finish();
        let dirty_rect = DeviceUintRect::new(
            DeviceUintPoint::new(30, 10),
            DeviceUintSize::new(1, 31));
        merge_blob_images(&result, &buf2, dirty_rect);
    }
}
