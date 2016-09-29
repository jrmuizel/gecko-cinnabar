use std::path::PathBuf;
use webrender_traits::{PipelineId, RenderApi, DisplayListBuilder};
use webrender_traits::{AuxiliaryListsBuilder, StackingContextId, DisplayListId};
use renderer::{Renderer, RendererOptions};
extern crate webrender_traits;


//extern crate glutin;

use app_units::Au;
use euclid::{Size2D, Point2D, Rect, Matrix4D};
use gleam::gl;
use std::ffi::CStr;
use webrender_traits::{ServoStackingContextId};
use webrender_traits::{Epoch, ColorF, FragmentType, GlyphInstance};
use webrender_traits::{ImageFormat, ImageKey, ImageRendering};
use std::fs::File;
use std::io::Read;
use std::env;
use std::mem;
use std::str::FromStr;
use std::slice;

use core_foundation::base::TCFType;
use core_foundation::string::CFString;
use core_foundation::bundle::{CFBundleGetBundleWithIdentifier, CFBundleGetFunctionPointerForName};

/*
struct Notifier {
    window_proxy: glutin::WindowProxy,
}

impl Notifier {
    fn new(window_proxy: glutin::WindowProxy) -> Notifier {
        Notifier {
            window_proxy: window_proxy,
        }
    }
}*/
pub struct WebRenderFrameBuilder {
    pub stacking_contexts: Vec<(StackingContextId, webrender_traits::StackingContext)>,
    pub display_lists: Vec<(DisplayListId, webrender_traits::BuiltDisplayList)>,
    pub auxiliary_lists_builder: AuxiliaryListsBuilder,
    pub root_pipeline_id: PipelineId,
    pub next_scroll_layer_id: usize,
}

impl WebRenderFrameBuilder {
    pub fn new(root_pipeline_id: PipelineId) -> WebRenderFrameBuilder {
        WebRenderFrameBuilder {
            stacking_contexts: vec![],
            display_lists: vec![],
            auxiliary_lists_builder: AuxiliaryListsBuilder::new(),
            root_pipeline_id: root_pipeline_id,
            next_scroll_layer_id: 0,
        }
    }

    pub fn add_stacking_context(&mut self,
                                api: &mut webrender_traits::RenderApi,
                                pipeline_id: PipelineId,
                                stacking_context: webrender_traits::StackingContext)
                                -> StackingContextId {
        assert!(pipeline_id == self.root_pipeline_id);
        let id = api.next_stacking_context_id();
        self.stacking_contexts.push((id, stacking_context));
        id
    }

    pub fn add_display_list(&mut self,
                            api: &mut webrender_traits::RenderApi,
                            display_list: webrender_traits::BuiltDisplayList,
                            stacking_context: &mut webrender_traits::StackingContext)
                            -> DisplayListId {
        let id = api.next_display_list_id();
        stacking_context.has_stacking_contexts = stacking_context.has_stacking_contexts ||
                                                 display_list.descriptor().has_stacking_contexts;
        stacking_context.display_lists.push(id);
        self.display_lists.push((id, display_list));
        id
    }

    pub fn next_scroll_layer_id(&mut self) -> webrender_traits::ScrollLayerId {
        let scroll_layer_id = self.next_scroll_layer_id;
        self.next_scroll_layer_id += 1;
        webrender_traits::ScrollLayerId::new(self.root_pipeline_id, scroll_layer_id)
    }

}
/*
impl webrender_traits::RenderNotifier for Notifier {
    fn new_frame_ready(&mut self) {
        self.window_proxy.wakeup_event_loop();
    }
    fn new_scroll_frame_ready(&mut self, composite_needed: bool) {
        self.window_proxy.wakeup_event_loop();
    }

    fn pipeline_size_changed(&mut self,
                             _: PipelineId,
                             _: Option<Size2D<f32>>) {
    }
}*/
pub struct WrState {
        size: (u32, u32),
        pipeline_id: PipelineId,
        renderer: Renderer,
        z_index: i32,
        api: webrender_traits::RenderApi,
        frame_builder: WebRenderFrameBuilder,
        dl_builder: Vec<DisplayListBuilder>,
}

#[cfg(target_os="macos")]
fn get_proc_address(addr: &str) -> *const () {
    let symbol_name: CFString = FromStr::from_str(addr).unwrap();
    let framework_name: CFString = FromStr::from_str("com.apple.opengl").unwrap();
    let framework = unsafe {
        CFBundleGetBundleWithIdentifier(framework_name.as_concrete_TypeRef())
    };
    let symbol = unsafe {
        CFBundleGetFunctionPointerForName(framework, symbol_name.as_concrete_TypeRef())
    };
    symbol as *const _
}
 
#[no_mangle]
pub extern fn wr_create(width: u32, height: u32, counter: u32) -> *mut WrState {
  println!("Test");
  // hack to find the directory for the shaders
  let res_path = concat!(env!("CARGO_MANIFEST_DIR"),"/res");

  gl::load_with(|symbol| get_proc_address(symbol) as *const _); 
  gl::clear_color(0.3, 0.0, 0.0, 1.0);

  let version = unsafe {
    let data = CStr::from_ptr(gl::GetString(gl::VERSION) as *const _).to_bytes().to_vec();
    String::from_utf8(data).unwrap()
  };  

  println!("OpenGL version new {}", version);
  println!("Shader resource path: {}", res_path);

    let opts = RendererOptions {
        device_pixel_ratio: 1.0,
        resource_path: PathBuf::from(res_path),
        enable_aa: false,
        enable_msaa: false,
        enable_profiler: true,
        enable_recording: false,
        enable_scrollbars: false,
        precache_shaders: false,
        debug: false,
    };

    let (mut renderer, sender) = Renderer::new(opts);
    let mut api = sender.create_api();

//     let font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
//     let font_bytes = load_file(font_path);
//     let font_key = api.add_raw_font(font_bytes);

    // let notifier = Box::new(Notifier::new(window.create_window_proxy()));
    // renderer.set_render_notifier(notifier);

    let pipeline_id = PipelineId(0, counter);

    let builder = WebRenderFrameBuilder::new(pipeline_id);

  let mut state = Box::new(WrState {
    size: (width, height),
    pipeline_id: pipeline_id,
    renderer: renderer,
    z_index: 0,
    api: api,
    frame_builder: builder,
    dl_builder: Vec::new(),
  });

  Box::into_raw(state)
}

#[no_mangle]
pub extern fn wr_dp_begin(state:&mut WrState, width: u32, height: u32) {
    state.size = (width, height);
    state.dl_builder.clear();
    state.z_index = 0;
    state.dl_builder.push(webrender_traits::DisplayListBuilder::new());

}

#[no_mangle]
pub extern fn wr_push_dl_builder(state:&mut WrState)
{
    state.dl_builder.push(webrender_traits::DisplayListBuilder::new());
}

#[no_mangle]
pub extern fn wr_pop_dl_builder(state:&mut WrState, x: f32, y: f32, width: f32, height: f32, transform: &Matrix4D<f32>)
{
    // 
    let servo_id = ServoStackingContextId(FragmentType::FragmentBody, 0);
    state.z_index += 1;

    let mut sc =
        webrender_traits::StackingContext::new(servo_id,
                                               None,
                                               webrender_traits::ScrollPolicy::Scrollable,
                                               Rect::new(Point2D::new(0., 0.), Size2D::new(0., 0.)),
                                               Rect::new(Point2D::new(x, y), Size2D::new(width + 1000., height + 1000.)),
                                               state.z_index,
                                               transform,
                                               &Matrix4D::identity(),
                                               false,
                                               webrender_traits::MixBlendMode::Normal,
                                               Vec::new(),
                                               &mut state.frame_builder.auxiliary_lists_builder);
    let dl = state.dl_builder.pop().unwrap();
    state.frame_builder.add_display_list(&mut state.api, dl.finalize(), &mut sc);
    let pipeline_id = state.frame_builder.root_pipeline_id;
    let stacking_context_id = state.frame_builder.add_stacking_context(&mut state.api, pipeline_id, sc);

    state.dl_builder.last_mut().unwrap().push_stacking_context(stacking_context_id);

}

#[no_mangle]
pub extern fn wr_dp_end(state:&mut WrState) {
    let epoch = Epoch(0);
    let root_background_color = ColorF::new(0.3, 0.0, 0.0, 1.0);
    let pipeline_id = state.pipeline_id;
    let (width, height) = state.size;
    let bounds = Rect::new(Point2D::new(0.0, 0.0), Size2D::new(width as f32, height as f32));
    let root_scroll_layer_id = state.frame_builder.next_scroll_layer_id();
    let servo_id = ServoStackingContextId(FragmentType::FragmentBody, 0);

    let mut sc =
        webrender_traits::StackingContext::new(servo_id,
                                               Some(root_scroll_layer_id),
                                               webrender_traits::ScrollPolicy::Scrollable,
                                               bounds,
                                               bounds,
                                               0,
                                               &Matrix4D::identity(),
                                               &Matrix4D::identity(),
                                               true,
                                               webrender_traits::MixBlendMode::Normal,
                                               Vec::new(),
                                               &mut state.frame_builder.auxiliary_lists_builder);

    assert!(state.dl_builder.len() == 1);
    let dl = state.dl_builder.pop().unwrap();
    state.frame_builder.add_display_list(&mut state.api, dl.finalize(), &mut sc);
    let sc_id = state.frame_builder.add_stacking_context(&mut state.api, pipeline_id, sc);

    let fb = mem::replace(&mut state.frame_builder, WebRenderFrameBuilder::new(pipeline_id));

    state.api.set_root_stacking_context(sc_id,
                                  root_background_color,
                                  epoch,
                                  pipeline_id,
                                  Size2D::new(width as f32, height as f32),
                                  fb.stacking_contexts,
                                  fb.display_lists,
                                  fb.auxiliary_lists_builder
                                               .finalize());

    state.api.set_root_pipeline(pipeline_id);

    gl::clear(gl::COLOR_BUFFER_BIT);
    state.renderer.update();

    state.renderer.render(Size2D::new(width, height));
}

#[no_mangle]
pub extern fn wr_add_image(state:&mut WrState, width: u32, height: u32, format: ImageFormat, bytes: * const u8, size: usize) -> ImageKey {
    let bytes = unsafe { slice::from_raw_parts(bytes, size).to_owned() };
    state.api.add_image(width, height, format, bytes)
}

pub extern fn wr_delete_image(state:&mut WrState, key: ImageKey) {
    state.api.delete_image(key)
}


#[no_mangle]
pub extern fn wr_dp_push_rect(state:&mut WrState, x: f32, y: f32, w: f32, h: f32, r: f32, g: f32, b: f32, a: f32) {
    if state.dl_builder.len() == 0 {
      return;
    }
    let (width, height) = state.size;
    let bounds = Rect::new(Point2D::new(x, y), Size2D::new(width as f32, height as f32));
    let clip_region = webrender_traits::ClipRegion::new(&bounds,
                                                        Vec::new(),
                                                        &mut state.frame_builder.auxiliary_lists_builder);
    state.dl_builder.last_mut().unwrap().push_rect(Rect::new(Point2D::new(x, y), Size2D::new(w, h)),
                               clip_region,
                               ColorF::new(r, g, b, a));
}

#[no_mangle]
pub extern fn wr_dp_push_image(state:&mut WrState, x: f32, y: f32, w: f32, h: f32, key: ImageKey) {
    if state.dl_builder.len() == 0 {
      return;
    }
    let (width, height) = state.size;
    let bounds = Rect::new(Point2D::new(x, y), Size2D::new(width as f32, height as f32));
    let clip_region = webrender_traits::ClipRegion::new(&bounds,
                                                        Vec::new(),
                                                        &mut state.frame_builder.auxiliary_lists_builder);
    let rect = Rect::new(Point2D::new(x, y), Size2D::new(w, h));
    state.dl_builder.last_mut().unwrap().push_image(rect,
                               clip_region,
                               rect.size,
                               rect.size,
                               ImageRendering::Auto,
                               key);
}

#[no_mangle]
pub extern fn wr_destroy(state:*mut WrState) {
  unsafe {
    Box::from_raw(state);
  }
}

#[no_mangle]
pub extern fn wr_init() {
}
