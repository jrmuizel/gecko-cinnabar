extern void* gWRState;

extern "C" void* wr_create();
extern "C" void wr_render(void* wrstate);
extern "C" void wr_destroy(void* wrstate);

extern "C" void wr_dp_begin(void* wrState);
extern "C" void wr_dp_end(void* wrState);
extern "C" void wr_dp_push_rect(void* wrState, float x, float y, float w, float h, float r, float g, float b, float a);
