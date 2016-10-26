/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#![feature(step_by)]
//#![feature(mpsc_select)]

//! A GPU based Webrender.
//!
//! It serves as an experimental render backend for [Servo](https://servo.org/),
//! but it can also be used as such in a standalone application.
//!
//! # External dependencies
//! Webrender currently depends on [FreeType](https://www.freetype.org/)
//!
//! # Api Structure
//! The main entry point to webrender is the `webrender::renderer::Renderer`.
//!
//! By calling `Renderer::new(...)` you get a `Renderer`, as well as a `RenderApiSender`.
//! Your `Renderer` is responsible to render the previously processed frames onto the screen.
//!
//! By calling `yourRenderApiSenderInstance.create_api()`, you'll get a `RenderApi` instance,
//! which is responsible for the processing of new frames. A worker thread is used internally to
//! untie the workload from the application thread and therefore be able
//! to make better use of multicore systems.
//!
//! What is referred to as a `frame`, is the current geometry on the screen.
//! A new Frame is created by calling [set_root_stacking_context()][newframe] on the `RenderApi`.
//! When the geometry is processed, the application will be informed via a `RenderNotifier`,
//! a callback which you employ with [set_render_notifier][notifier] on the `Renderer`
//! More information about [stacking contexts][stacking_contexts].
//!
//! `set_root_stacking_context()` also needs to be supplied with `BuiltDisplayList`s.
//! These are obtained by finalizing a `DisplayListBuilder`. These are used to draw your geometry.
//! But it doesn't only contain trivial geometry, it can also store another StackingContext, as
//! they're nestable.
//!
//! Remember to insert the DisplayListId into the StackingContext as well, as they'll be referenced
//! from there. An Id for your DisplayList can be obtained by calling
//! `yourRenderApiInstance.next_display_list_id();`
//!
//! [stacking_contexts]: https://developer.mozilla.org/en-US/docs/Web/CSS/CSS_Positioning/Understanding_z_index/The_stacking_context
//! [newframe]: ../webrender_traits/struct.RenderApi.html#method.set_root_stacking_context
//! [notifier]: struct.Renderer.html#method.set_render_notifier

#[macro_use]
extern crate lazy_static;
#[macro_use]
extern crate log;

mod batch;
mod batch_builder;
mod debug_font_data;
mod debug_render;
mod device;
mod frame;
mod freelist;
mod geometry;
mod internal_types;
mod layer;
mod profiler;
mod render_backend;
mod resource_cache;
mod resource_list;
mod scene;
mod spring;
mod texture_cache;
mod tiling;
mod util;

mod platform {

    #[cfg(target_os="macos")]
    pub use platform::macos::font;
    #[cfg(any(target_os="linux", target_os="android", target_os = "windows"))]
    pub use platform::linux::font;

    #[cfg(target_os="macos")]
    pub mod macos {
        pub mod font;
    }
    #[cfg(any(target_os="linux", target_os="android", target_os = "windows"))]
    pub mod linux {
        pub mod font;
    }
}

pub mod renderer;

#[cfg(target_os="macos")]
extern crate core_graphics;
#[cfg(target_os="macos")]
extern crate core_text;

#[cfg(not(target_os="macos"))]
extern crate freetype;

extern crate app_units;
extern crate euclid;
extern crate fnv;
extern crate gleam;
extern crate ipc_channel;
extern crate num_traits;
//extern crate notify;
extern crate scoped_threadpool;
extern crate time;
extern crate webrender_traits;
extern crate offscreen_gl_context;
extern crate byteorder;

pub use renderer::{Renderer, RendererOptions};

extern crate glutin;

use app_units::Au;
use euclid::{Size2D, Point2D, Rect, Matrix4D};
use gleam::gl;
use std::path::PathBuf;
use std::ffi::CStr;
use webrender_traits::{PipelineId, ServoStackingContextId, StackingContextId, DisplayListId};
use webrender_traits::{AuxiliaryListsBuilder, Epoch, ColorF, FragmentType, GlyphInstance};
use std::fs::File;
use std::io::Read;
use std::env;
use webrender_traits::DisplayListBuilder;
use std::mem;

struct Notifier {
    window_proxy: glutin::WindowProxy,
}

impl Notifier {
    fn new(window_proxy: glutin::WindowProxy) -> Notifier {
        Notifier {
            window_proxy: window_proxy,
        }
    }
}
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
}
pub struct wrstate {
        window: glutin::Window,
        renderer: Renderer,
        api: webrender_traits::RenderApi,
        frame_builder: WebRenderFrameBuilder,
        dl_builder: Vec<DisplayListBuilder>,
}
 
#[no_mangle]
pub extern fn wr_create() -> *mut wrstate {
  println!("Test");
  let res_path = "/Users/jrmuizel/source/webrender/gfx/webrenderer/res";

  let window = glutin::WindowBuilder::new().with_dimensions(1024, 1024).with_gl(glutin::GlRequest::Specific(glutin::Api::OpenGl, (3, 2))).build().unwrap();

  unsafe {
    window.make_current().ok();
    gl::load_with(|symbol| window.get_proc_address(symbol) as *const _); 
    gl::clear_color(0.3, 0.0, 0.0, 1.0);
  }   

  let version = unsafe {
    let data = CStr::from_ptr(gl::GetString(gl::VERSION) as *const _).to_bytes().to_vec();
    String::from_utf8(data).unwrap()
  };  

  println!("OpenGL version new {}", version);
  println!("Shader resource path: {}", res_path);

    let (width, height) = window.get_inner_size().unwrap();

    let opts = RendererOptions {
        device_pixel_ratio: 1.0,
        resource_path: PathBuf::from(res_path),
        enable_aa: false,
        enable_msaa: false,
        enable_profiler: true,
        debug: false,
    };

    let (mut renderer, sender) = renderer::Renderer::new(opts);
    let mut api = sender.create_api();

//     let font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
//     let font_bytes = load_file(font_path);
//     let font_key = api.add_raw_font(font_bytes);

    let notifier = Box::new(Notifier::new(window.create_window_proxy()));
    renderer.set_render_notifier(notifier);

    let pipeline_id = PipelineId(0, 0);

    let builder = WebRenderFrameBuilder::new(pipeline_id);

  let mut state = Box::new(wrstate {
    window: window,
    renderer: renderer,
    api: api,
    frame_builder: builder,
    dl_builder: Vec::new(),
  });

  Box::into_raw(state)
}

#[no_mangle]
pub extern fn wr_dp_begin(state:&mut wrstate) {
    state.dl_builder.clear();
    state.dl_builder.push(webrender_traits::DisplayListBuilder::new());

}

#[no_mangle]
pub extern fn wr_dp_end(state:&mut wrstate) {
    let epoch = Epoch(0);
    let root_background_color = ColorF::new(0.3, 0.0, 0.0, 1.0);
    let pipeline_id = PipelineId(0, 0);
    let (width, height) = state.window.get_inner_size().unwrap();
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

    state.window.swap_buffers().ok();
}


#[no_mangle]
pub extern fn wr_dp_push_rect(state:&mut wrstate, x: f32, y: f32, w: f32, h: f32, r: f32, g: f32, b: f32, a: f32) {
    if state.dl_builder.len() == 0 {
      return;
    }
    let (width, height) = state.window.get_inner_size().unwrap();
    let bounds = Rect::new(Point2D::new(0.0, 0.0), Size2D::new(width as f32, height as f32));
    let clip_region = webrender_traits::ClipRegion::new(&bounds,
                                                        Vec::new(),
                                                        &mut state.frame_builder.auxiliary_lists_builder);
    state.dl_builder.last_mut().unwrap().push_rect(Rect::new(Point2D::new(x, y), Size2D::new(w, h)),
                               clip_region,
                               ColorF::new(r, g, b, a));
}

#[no_mangle]
pub extern fn wr_render(state:&mut wrstate) {

    state.dl_builder.clear();
    state.dl_builder.push(webrender_traits::DisplayListBuilder::new());

    let epoch = Epoch(0);
    let root_background_color = ColorF::new(0.3, 0.0, 0.0, 1.0);
    let pipeline_id = PipelineId(0, 0);
    let (width, height) = state.window.get_inner_size().unwrap();
    let root_scroll_layer_id = state.frame_builder.next_scroll_layer_id();


    let bounds = Rect::new(Point2D::new(0.0, 0.0), Size2D::new(width as f32, height as f32));

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

    let clip_region = webrender_traits::ClipRegion::new(&bounds,
                                                        Vec::new(),
                                                        &mut state.frame_builder.auxiliary_lists_builder);

    state.dl_builder.last_mut().unwrap().push_rect(Rect::new(Point2D::new(100.0, 100.0), Size2D::new(100.0, 100.0)),
                      clip_region,
                      ColorF::new(0.0, 1.0, 0.0, 1.0));

    let text_bounds = Rect::new(Point2D::new(100.0, 200.0), Size2D::new(700.0, 300.0));

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

    state.window.swap_buffers().ok();
}

#[no_mangle]
pub extern fn wr_destroy(state:*mut wrstate) {
  unsafe {
    Box::from_raw(state);
  }
}

#[no_mangle]
pub extern fn wr_init() {

    // NB: rust &str aren't null terminated.
    let greeting = "hello from rust.\0";
}
