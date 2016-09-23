struct wrstate;
extern "C" wrstate* wr_create(uint32_t width, uint32_t height, uint32_t counter);
extern "C" void wr_render(wrstate* wrstate);
extern "C" void wr_destroy(wrstate* wrstate);

extern "C" void wr_dp_begin(wrstate* wrState, uint32_t width, uint32_t height);
extern "C" void wr_dp_end(wrstate* wrState);
extern "C" void wr_dp_push_rect(wrstate* wrState, float x, float y, float w, float h, float r, float g, float b, float a);
