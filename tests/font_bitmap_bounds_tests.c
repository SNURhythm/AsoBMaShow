// Exercise the actual vendored COLR implementation with synthetic glyph bounds.
// The allocator refuses every request, so even the unfixed code cannot copy into
// an undersized allocation or consume a large amount of memory.
#define FT2_BUILD_LIBRARY
#include "../SDL_ttf/external/freetype/src/sfnt/sfnt.c"

#include <stdio.h>

static unsigned allocation_calls;
static long allocation_size;

static void *refuse_allocation(FT_Memory memory, long size) {
  (void)memory;
  ++allocation_calls;
  allocation_size = size;
  return NULL;
}

int main(void) {
  struct FT_MemoryRec_ memory = {0};
  TT_FaceRec face = {0};
  FT_GlyphSlotRec destination = {0}, source = {0};
  unsigned char pixel[4] = {0};
  memory.alloc = refuse_allocation;
  face.root.memory = &memory;
  destination.bitmap.buffer = pixel;
  destination.bitmap.width = destination.bitmap.rows = 1;
  destination.bitmap.pitch = 4;
  destination.bitmap_top = 1;
  source.bitmap.width = source.bitmap.rows = 1;
  source.bitmap_left = 16383;
  source.bitmap_top = 65537;
  // The combined bitmap is 16384 * 65537 * 4 bytes. Multiplication in
  // 32-bit unsigned arithmetic incorrectly requests only 65536 bytes.
  const FT_Error result = tt_face_colr_blend_layer(&face, 0, &destination, &source);
  if (result != FT_Err_Array_Too_Large || allocation_calls != 0) {
    fprintf(stderr, "COLR bounds were not rejected before allocation: error=%d, calls=%u, bytes=%ld\n",
            result, allocation_calls, allocation_size);
    return 1;
  }
  return 0;
}
