#include <GL/glew.h>
#include <SDL.h>
#include <SDL_opengl.h>
#include "core/assert.h"
#include "core/profiler.h"
#include "renderer/backend.h"
#include "ui/window.h"

static const int MAX_TEXTURES = 1024;

typedef enum {
  MAP_DIFFUSE,
} texture_map_t;

typedef enum {
  UNIFORM_MODELVIEWPROJECTIONMATRIX,
  UNIFORM_DIFFUSEMAP,
  UNIFORM_NUM_UNIFORMS
} uniform_attr_t;

typedef struct {
  GLuint program;
  GLuint vertex_shader;
  GLuint fragment_shader;
  GLint uniforms[UNIFORM_NUM_UNIFORMS];
} shader_program_t;

typedef struct rb_s {
  struct window_s *window;
  SDL_GLContext ctx;
  bool debug_wireframe;

  // resources
  GLuint textures[MAX_TEXTURES];
  GLuint white_tex;

  shader_program_t ta_program;
  shader_program_t ui_program;

  GLuint ta_vao;
  GLuint ta_vbo;
  GLuint ui_vao;
  GLuint ui_vbo;
  GLuint ui_ibo;
  bool ui_use_ibo;

  // current gl state
  bool scissor_test;
  bool depth_mask;
  depth_func_t depth_func;
  cull_face_t cull_face;
  blend_func_t src_blend;
  blend_func_t dst_blend;
  GLuint current_vao;
  int vertex_attribs;
  shader_program_t *current_program;
} rb_t;

#include "renderer/ta.glsl"
#include "renderer/ui.glsl"

static const int GLSL_VERSION = 330;

// must match order of uniform_attr_t enum
static const char *uniform_names[] = {"u_mvp",  //
                                      "u_diffuse_map"};

static GLenum filter_funcs[] = {
    GL_NEAREST,                // FILTER_NEAREST
    GL_LINEAR,                 // FILTER_BILINEAR
    GL_NEAREST_MIPMAP_LINEAR,  // FILTER_NEAREST + mipmaps
    GL_LINEAR_MIPMAP_LINEAR    // FILTER_BILINEAR + mipmaps
};

static GLenum wrap_modes[] = {
    GL_REPEAT,          // WRAP_REPEAT
    GL_CLAMP_TO_EDGE,   // WRAP_CLAMP_TO_EDGE
    GL_MIRRORED_REPEAT  // WRAP_MIRRORED_REPEAT
};

static GLenum depth_funcs[] = {
    GL_NONE,      // DEPTH_NONE
    GL_NEVER,     // DEPTH_NEVER
    GL_LESS,      // DEPTH_LESS
    GL_EQUAL,     // DEPTH_EQUAL
    GL_LEQUAL,    // DEPTH_LEQUAL
    GL_GREATER,   // DEPTH_GREATER
    GL_NOTEQUAL,  // DEPTH_NEQUAL
    GL_GEQUAL,    // DEPTH_GEQUAL
    GL_ALWAYS     // DEPTH_ALWAYS
};

static GLenum cull_face[] = {
    GL_NONE,   // CULL_NONE
    GL_FRONT,  // CULL_FRONT
    GL_BACK    // CULL_BACK
};

static GLenum blend_funcs[] = {GL_NONE,
                               GL_ZERO,
                               GL_ONE,
                               GL_SRC_COLOR,
                               GL_ONE_MINUS_SRC_COLOR,
                               GL_SRC_ALPHA,
                               GL_ONE_MINUS_SRC_ALPHA,
                               GL_DST_ALPHA,
                               GL_ONE_MINUS_DST_ALPHA,
                               GL_DST_COLOR,
                               GL_ONE_MINUS_DST_COLOR};

static GLenum prim_types[] = {
    GL_TRIANGLES,  // PRIM_TRIANGLES
    GL_LINES,      // PRIM_LINES
};

static bool rb_init_context(rb_t *rb);
static void rb_destroy_context(rb_t *rb);
static void rb_create_textures(rb_t *rb);
static void rb_destroy_textures(rb_t *rb);
static void rb_create_shaders(rb_t *rb);
static void rb_destroy_shaders(rb_t *rb);
static void rb_create_vertex_buffers(rb_t *rb);
static void rb_destroy_vertex_buffers(rb_t *rb);

static void rb_set_initial_state(rb_t *rb);
static void rb_set_scissor_test(rb_t *rb, bool enabled);
static void rb_set_scissor_clip(rb_t *rb, int x, int y, int width, int height);
static void rb_set_depth_mask(rb_t *rb, bool enabled);
static void rb_set_depth_func(rb_t *rb, depth_func_t fn);
static void rb_set_cull_face(rb_t *rb, cull_face_t fn);
static void rb_set_blend_func(rb_t *rb, blend_func_t src_fn,
                              blend_func_t dst_fn);

static void rb_bind_vao(rb_t *rb, GLuint vao);
static void rb_bind_program(rb_t *rb, shader_program_t *program);
static void rb_bind_texture(rb_t *rb, texture_map_t map, GLuint tex);
static GLint rb_get_uniform(rb_t *rb, uniform_attr_t attr);

static bool rb_compile_program(shader_program_t *program, const char *header,
                               const char *vertexSource,
                               const char *fragmentSource);
static void rb_destroy_program(shader_program_t *program);

rb_t *rb_create(struct window_s *window) {
  rb_t *rb = (rb_t *)calloc(1, sizeof(rb_t));

  rb->window = window;
  // rb->window->AddListener(this);

  if (!rb_init_context(rb)) {
    rb_destroy(rb);
    return NULL;
  }

  rb_create_textures(rb);
  rb_create_shaders(rb);
  rb_create_vertex_buffers(rb);
  rb_set_initial_state(rb);

  return rb;
}

void rb_destroy(rb_t *rb) {
  rb_destroy_vertex_buffers(rb);
  rb_destroy_shaders(rb);
  rb_destroy_textures(rb);
  rb_destroy_context(rb);

  // rb->window->RemoveListener(this);
}

texture_handle_t rb_register_texture(rb_t *rb, pxl_format_t format,
                                     filter_mode_t filter, wrap_mode_t wrap_u,
                                     wrap_mode_t wrap_v, bool mipmaps,
                                     int width, int height,
                                     const uint8_t *buffer) {
  // FIXME worth speeding up?
  texture_handle_t handle;
  for (handle = 1; handle < MAX_TEXTURES; handle++) {
    if (!rb->textures[handle]) {
      break;
    }
  }
  CHECK_LT(handle, MAX_TEXTURES);

  GLuint internal_fmt;
  GLuint pixel_fmt;
  switch (format) {
    case PXL_RGBA:
      internal_fmt = GL_RGBA;
      pixel_fmt = GL_UNSIGNED_BYTE;
      break;
    case PXL_RGBA5551:
      internal_fmt = GL_RGBA;
      pixel_fmt = GL_UNSIGNED_SHORT_5_5_5_1;
      break;
    case PXL_RGB565:
      internal_fmt = GL_RGB;
      pixel_fmt = GL_UNSIGNED_SHORT_5_6_5;
      break;
    case PXL_RGBA4444:
      internal_fmt = GL_RGBA;
      pixel_fmt = GL_UNSIGNED_SHORT_4_4_4_4;
      break;
    case PXL_RGBA8888:
      internal_fmt = GL_RGBA;
      pixel_fmt = GL_UNSIGNED_INT_8_8_8_8;
      break;
    default:
      LOG_FATAL("Unexpected pixel format %d", format);
      break;
  }

  GLuint *gltex = &rb->textures[handle];
  glGenTextures(1, gltex);
  glBindTexture(GL_TEXTURE_2D, *gltex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  filter_funcs[mipmaps * NUM_FILTER_MODES + filter]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter_funcs[filter]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_modes[wrap_u]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_modes[wrap_v]);
  glTexImage2D(GL_TEXTURE_2D, 0, internal_fmt, width, height, 0, internal_fmt,
               pixel_fmt, buffer);

  if (mipmaps) {
    glGenerateMipmap(GL_TEXTURE_2D);
  }

  glBindTexture(GL_TEXTURE_2D, 0);

  return handle;
}

void rb_free_texture(rb_t *rb, texture_handle_t handle) {
  GLuint *gltex = &rb->textures[handle];
  glDeleteTextures(1, gltex);
  *gltex = 0;
}

void rb_begin_frame(rb_t *rb) {
  int width = win_width(rb->window);
  int height = win_height(rb->window);

  rb_set_depth_mask(rb, true);

  glViewport(0, 0, width, height);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void rb_end_frame(rb_t *rb) {
  SDL_GL_SwapWindow(win_handle(rb->window));
}

void rb_begin2d(rb_t *rb) {
  float ortho[16];

  ortho[0] = 2.0f / (float)win_width(rb->window);
  ortho[4] = 0.0f;
  ortho[8] = 0.0f;
  ortho[12] = -1.0f;

  ortho[1] = 0.0f;
  ortho[5] = -2.0f / (float)win_height(rb->window);
  ortho[9] = 0.0f;
  ortho[13] = 1.0f;

  ortho[2] = 0.0f;
  ortho[6] = 0.0f;
  ortho[10] = 0.0f;
  ortho[14] = 0.0f;

  ortho[3] = 0.0f;
  ortho[7] = 0.0f;
  ortho[11] = 0.0f;
  ortho[15] = 1.0f;

  rb_set_depth_mask(rb, false);
  rb_set_depth_func(rb, DEPTH_NONE);
  rb_set_cull_face(rb, CULL_NONE);

  rb_bind_vao(rb, rb->ui_vao);
  rb_bind_program(rb, &rb->ui_program);
  glUniformMatrix4fv(rb_get_uniform(rb, UNIFORM_MODELVIEWPROJECTIONMATRIX), 1,
                     GL_FALSE, ortho);
  glUniform1i(rb_get_uniform(rb, UNIFORM_DIFFUSEMAP), MAP_DIFFUSE);
}

void rb_end2d(rb_t *rb) {
  rb_set_scissor_test(rb, false);
}

void rb_begin_surfaces2d(rb_t *rb, const vertex2d_t *verts, int num_verts,
                         uint16_t *indices, int num_indices) {
  glBindBuffer(GL_ARRAY_BUFFER, rb->ui_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertex2d_t) * num_verts, verts,
               GL_DYNAMIC_DRAW);

  if (indices) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rb->ui_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint16_t) * num_indices,
                 indices, GL_DYNAMIC_DRAW);
    rb->ui_use_ibo = true;
  } else {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, -1);
    rb->ui_use_ibo = false;
  }
}

void rb_draw_surface2d(rb_t *rb, const surface2d_t *surf) {
  if (surf->scissor) {
    rb_set_scissor_test(rb, true);
    rb_set_scissor_clip(rb, (int)surf->scissor_rect[0],
                        (int)surf->scissor_rect[1], (int)surf->scissor_rect[2],
                        (int)surf->scissor_rect[3]);
  } else {
    rb_set_scissor_test(rb, false);
  }

  rb_set_blend_func(rb, surf->src_blend, surf->dst_blend);
  rb_bind_texture(rb, MAP_DIFFUSE,
                  surf->texture ? rb->textures[surf->texture] : rb->white_tex);

  if (rb->ui_use_ibo) {
    glDrawElements(prim_types[surf->prim_type], surf->num_verts,
                   GL_UNSIGNED_SHORT,
                   (void *)(intptr_t)(sizeof(uint16_t) * surf->first_vert));
  } else {
    glDrawArrays(prim_types[surf->prim_type], surf->first_vert,
                 surf->num_verts);
  }
}

void rb_end_surfaces2d(rb_t *rb) {}

void rb_begin_surfaces(rb_t *rb, const float *projection, const vertex_t *verts,
                       int num_verts) {
  glBindBuffer(GL_ARRAY_BUFFER, rb->ta_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_t) * num_verts, verts,
               GL_DYNAMIC_DRAW);

  rb_bind_vao(rb, rb->ta_vao);
  rb_bind_program(rb, &rb->ta_program);
  glUniformMatrix4fv(rb_get_uniform(rb, UNIFORM_MODELVIEWPROJECTIONMATRIX), 1,
                     GL_FALSE, projection);
  glUniform1i(rb_get_uniform(rb, UNIFORM_DIFFUSEMAP), MAP_DIFFUSE);

  if (rb->debug_wireframe) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  }
}

void rb_draw_surface(rb_t *rb, const surface_t *surf) {
  rb_set_depth_mask(rb, surf->depth_write);
  rb_set_depth_func(rb, surf->depth_func);
  rb_set_cull_face(rb, surf->cull);
  rb_set_blend_func(rb, surf->src_blend, surf->dst_blend);

  // TODO use surf->shade to select correct shader

  rb_bind_texture(rb, MAP_DIFFUSE,
                  surf->texture ? rb->textures[surf->texture] : rb->white_tex);
  glDrawArrays(GL_TRIANGLE_STRIP, surf->first_vert, surf->num_verts);
}

void rb_end_surfaces(rb_t *rb) {
  if (rb->debug_wireframe) {
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  }
}

// void GLBackend::OnPaint(bool show_main_menu) {
//   if (show_main_menu && ImGui::BeginMainMenuBar()) {
//     if (ImGui::BeginMenu("Render")) {
//       ImGui::MenuItem("Wireframe", "", &rb->debug_wireframe);
//       ImGui::EndMenu();
//     }

//     ImGui::EndMainMenuBar();
//   }
// }

bool rb_init_context(rb_t *rb) {
  // need at least a 3.3 core context for our shaders
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

  // request a 24-bit depth buffer. 16-bits isn't enough precision when
  // unprojecting dreamcast coordinates, see tr_proj_mat
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

  rb->ctx = SDL_GL_CreateContext(win_handle(rb->window));
  if (!rb->ctx) {
    LOG_WARNING("OpenGL context creation failed: %s", SDL_GetError());
    return false;
  }

  // link in gl functions at runtime
  glewExperimental = GL_TRUE;
  GLenum err = glewInit();
  if (err != GLEW_OK) {
    LOG_WARNING("GLEW initialization failed: %s", glewGetErrorString(err));
    return false;
  }

  // enable vsync
  SDL_GL_SetSwapInterval(1);

  return true;
}

void rb_destroy_context(rb_t *rb) {
  if (!rb->ctx) {
    return;
  }

  SDL_GL_DeleteContext(rb->ctx);
  rb->ctx = NULL;
}

void rb_create_textures(rb_t *rb) {
  uint8_t pixels[64 * 64 * 4];
  memset(pixels, 0xff, sizeof(pixels));
  glGenTextures(1, &rb->white_tex);
  glBindTexture(GL_TEXTURE_2D, rb->white_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void rb_destroy_textures(rb_t *rb) {
  if (!rb->ctx) {
    return;
  }

  glDeleteTextures(1, &rb->white_tex);

  for (int i = 1; i < MAX_TEXTURES; i++) {
    if (!rb->textures[i]) {
      continue;
    }
    glDeleteTextures(1, &rb->textures[i]);
  }
}

void rb_create_shaders(rb_t *rb) {
  if (!rb_compile_program(&rb->ta_program, NULL, ta_vp, ta_fp)) {
    LOG_FATAL("Failed to compile ta shader.");
  }

  if (!rb_compile_program(&rb->ui_program, NULL, ui_vp, ui_fp)) {
    LOG_FATAL("Failed to compile ui shader.");
  }
}

void rb_destroy_shaders(rb_t *rb) {
  if (!rb->ctx) {
    return;
  }

  rb_destroy_program(&rb->ta_program);
  rb_destroy_program(&rb->ui_program);
}

void rb_create_vertex_buffers(rb_t *rb) {
  //
  // UI vao
  //
  glGenVertexArrays(1, &rb->ui_vao);
  glBindVertexArray(rb->ui_vao);

  glGenBuffers(1, &rb->ui_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, rb->ui_vbo);

  glGenBuffers(1, &rb->ui_ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rb->ui_ibo);

  // xy
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vertex2d_t),
                        (void *)offsetof(vertex2d_t, xy));

  // texcoord
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex2d_t),
                        (void *)offsetof(vertex2d_t, uv));

  // color
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex2d_t),
                        (void *)offsetof(vertex2d_t, color));

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  //
  // TA vao
  //
  glGenVertexArrays(1, &rb->ta_vao);
  glBindVertexArray(rb->ta_vao);

  glGenBuffers(1, &rb->ta_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, rb->ta_vbo);

  // xyz
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex_t),
                        (void *)offsetof(vertex_t, xyz));

  // texcoord
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex_t),
                        (void *)offsetof(vertex_t, uv));

  // color
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex_t),
                        (void *)offsetof(vertex_t, color));

  // offset color
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex_t),
                        (void *)offsetof(vertex_t, offset_color));

  glBindVertexArray(0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void rb_destroy_vertex_buffers(rb_t *rb) {
  if (!rb->ctx) {
    return;
  }

  glDeleteBuffers(1, &rb->ui_ibo);
  glDeleteBuffers(1, &rb->ui_vbo);
  glDeleteVertexArrays(1, &rb->ui_vao);

  glDeleteBuffers(1, &rb->ta_vbo);
  glDeleteVertexArrays(1, &rb->ta_vao);
}

void rb_set_initial_state(rb_t *rb) {
  rb_set_depth_mask(rb, true);
  rb_set_depth_func(rb, DEPTH_NONE);
  rb_set_cull_face(rb, CULL_BACK);
  rb_set_blend_func(rb, BLEND_NONE, BLEND_NONE);
}

void rb_set_scissor_test(rb_t *rb, bool enabled) {
  if (rb->scissor_test == enabled) {
    return;
  }

  rb->scissor_test = enabled;

  if (enabled) {
    glEnable(GL_SCISSOR_TEST);
  } else {
    glDisable(GL_SCISSOR_TEST);
  }
}

void rb_set_scissor_clip(rb_t *rb, int x, int y, int width, int height) {
  glScissor(x, y, width, height);
}

void rb_set_depth_mask(rb_t *rb, bool enabled) {
  if (rb->depth_mask == enabled) {
    return;
  }

  rb->depth_mask = enabled;

  glDepthMask(enabled ? 1 : 0);
}

void rb_set_depth_func(rb_t *rb, depth_func_t fn) {
  if (rb->depth_func == fn) {
    return;
  }

  rb->depth_func = fn;

  if (fn == DEPTH_NONE) {
    glDisable(GL_DEPTH_TEST);
  } else {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(depth_funcs[fn]);
  }
}

void rb_set_cull_face(rb_t *rb, cull_face_t fn) {
  if (rb->cull_face == fn) {
    return;
  }

  rb->cull_face = fn;

  if (fn == CULL_NONE) {
    glDisable(GL_CULL_FACE);
  } else {
    glEnable(GL_CULL_FACE);
    glCullFace(cull_face[fn]);
  }
}

void rb_set_blend_func(rb_t *rb, blend_func_t src_fn, blend_func_t dst_fn) {
  if (rb->src_blend == src_fn && rb->dst_blend == dst_fn) {
    return;
  }

  rb->src_blend = src_fn;
  rb->dst_blend = dst_fn;

  if (src_fn == BLEND_NONE || dst_fn == BLEND_NONE) {
    glDisable(GL_BLEND);
  } else {
    glEnable(GL_BLEND);
    glBlendFunc(blend_funcs[src_fn], blend_funcs[dst_fn]);
  }
}

void rb_bind_vao(rb_t *rb, GLuint vao) {
  if (rb->current_vao == vao) {
    return;
  }

  rb->current_vao = vao;

  glBindVertexArray(vao);
}

void rb_bind_program(rb_t *rb, shader_program_t *program) {
  if (rb->current_program == program) {
    return;
  }

  rb->current_program = program;

  glUseProgram(program ? program->program : 0);
}

void rb_bind_texture(rb_t *rb, texture_map_t map, GLuint tex) {
  glActiveTexture(GL_TEXTURE0 + map);
  glBindTexture(GL_TEXTURE_2D, tex);
}

GLint rb_get_uniform(rb_t *rb, uniform_attr_t attr) {
  return rb->current_program->uniforms[attr];
}

static void rb_print_shader_log(GLuint shader) {
  int max_length, length;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &max_length);

  char *info_log = malloc(max_length);
  glGetShaderInfoLog(shader, max_length, &length, info_log);
  LOG_INFO(info_log);
  free(info_log);
}

static bool rb_compile_shader(const char *source, GLenum shader_type,
                              GLuint *shader) {
  size_t sourceLength = strlen(source);

  *shader = glCreateShader(shader_type);
  glShaderSource(*shader, 1, (const GLchar **)&source,
                 (const GLint *)&sourceLength);
  glCompileShader(*shader);

  GLint compiled;
  glGetShaderiv(*shader, GL_COMPILE_STATUS, &compiled);

  if (!compiled) {
    rb_print_shader_log(*shader);
    glDeleteShader(*shader);
    return false;
  }

  return true;
}

bool rb_compile_program(shader_program_t *program, const char *header,
                        const char *vertex_source,
                        const char *fragment_source) {
  char buffer[16384] = {0};

  memset(program, 0, sizeof(*program));
  program->program = glCreateProgram();

  if (vertex_source) {
    snprintf(buffer, sizeof(buffer) - 1, "#version %d\n%s%s", GLSL_VERSION,
             header ? header : "", vertex_source);
    buffer[sizeof(buffer) - 1] = 0;

    if (!rb_compile_shader(buffer, GL_VERTEX_SHADER, &program->vertex_shader)) {
      rb_destroy_program(program);
      return false;
    }

    glAttachShader(program->program, program->vertex_shader);
  }

  if (fragment_source) {
    snprintf(buffer, sizeof(buffer) - 1, "#version %d\n%s%s", GLSL_VERSION,
             header ? header : "", fragment_source);
    buffer[sizeof(buffer) - 1] = 0;

    if (!rb_compile_shader(buffer, GL_FRAGMENT_SHADER,
                           &program->fragment_shader)) {
      rb_destroy_program(program);
      return false;
    }

    glAttachShader(program->program, program->fragment_shader);
  }

  glLinkProgram(program->program);

  GLint linked;
  glGetProgramiv(program->program, GL_LINK_STATUS, &linked);

  if (!linked) {
    rb_destroy_program(program);
    return false;
  }

  for (int i = 0; i < UNIFORM_NUM_UNIFORMS; i++) {
    program->uniforms[i] =
        glGetUniformLocation(program->program, uniform_names[i]);
  }

  return true;
}

void rb_destroy_program(shader_program_t *program) {
  if (program->vertex_shader > 0) {
    glDeleteShader(program->vertex_shader);
  }

  if (program->fragment_shader > 0) {
    glDeleteShader(program->fragment_shader);
  }

  glDeleteProgram(program->program);
}
