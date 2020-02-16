//
// LICENSE:
//
// Copyright (c) 2016 -- 2019 Fabio Pellacini
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
#include "csg.h"
#include "parser.h"
//
#include "ext/yocto-gl/apps/yocto_opengl.h"
#include "ext/yocto-gl/yocto/yocto_common.h"
#include "ext/yocto-gl/yocto/yocto_commonio.h"
#include "ext/yocto-gl/yocto/yocto_sceneio.h"
#include "ext/yocto-gl/yocto/yocto_shape.h"
#include "ext/yocto-gl/yocto/yocto_trace.h"
using namespace yocto;

#include <future>
#include <memory>
using namespace std;

float get_seconds() { return get_time() * 1e-9; }

// Application state
struct app_state {
  // loading options
  string filename  = "scene.yaml";
  string imagename = "out.png";
  string name      = "";

  // options
  trace_params params            = {};
  int          preview_downscale = 6;

  // scene
  trace_scene scene = {};

  Csg   csg        = {};
  float timer      = 0;
  bool  add_skyenv = false;

  // rendering state
  trace_state  state    = {};
  image<vec4f> render   = {};
  image<vec4f> display  = {};
  float        exposure = 0;

  // view scene
  opengl_image        glimage  = {};
  draw_glimage_params glparams = {};

  // computation
  int          render_sample  = 0;
  atomic<bool> render_stop    = {};
  future<void> render_future  = {};
  int          render_counter = 0;

  // Enqueued commands
  vector<function<void()>> commands = {};

  void run(function<void()>&& f) { commands.push_back(f); }

  ~app_state() {
    render_stop = true;
    if (render_future.valid()) render_future.get();
  }
};

// construct a scene from io
void init_scene(trace_scene& scene, sceneio_model& ioscene) {
  scene = trace_scene{};

  for (auto& iocamera : ioscene.cameras) {
    add_camera(scene, iocamera.frame, iocamera.lens, iocamera.aspect,
        iocamera.film, iocamera.aperture, iocamera.focus);
  }

  for (auto& iotexture : ioscene.textures) {
    if (!iotexture.hdr.empty()) {
      add_texture(scene, std::move(iotexture.hdr));
    } else if (!iotexture.ldr.empty()) {
      add_texture(scene, std::move(iotexture.ldr));
    }
  }

  for (auto& iomaterial : ioscene.materials) {
    auto id = add_material(scene);
    set_material_emission(
        scene, id, iomaterial.emission, iomaterial.emission_tex);
    set_material_diffuse(scene, id, iomaterial.diffuse, iomaterial.diffuse_tex);
    set_material_specular(
        scene, id, iomaterial.specular, iomaterial.specular_tex);
    set_material_metallic(
        scene, id, iomaterial.metallic, iomaterial.metallic_tex);
    set_material_transmission(
        scene, id, iomaterial.transmission, iomaterial.transmission_tex);
    set_material_roughness(
        scene, id, iomaterial.roughness, iomaterial.roughness_tex);
    set_material_opacity(scene, id, iomaterial.opacity, iomaterial.opacity_tex);
    set_material_refract(scene, id, iomaterial.refract);
    set_material_normalmap(scene, id, iomaterial.normal_tex);
    set_material_volume(scene, id, iomaterial.volemission,
        iomaterial.voltransmission, iomaterial.volmeanfreepath,
        iomaterial.volscatter, iomaterial.volscale, iomaterial.volanisotropy,
        iomaterial.subsurface_tex);
  }

  for (auto& iosubdiv : ioscene.subdivs) {
    tesselate_subdiv(ioscene, iosubdiv);
    iosubdiv = {};
  }

  for (auto& ioshape : ioscene.shapes) {
    if (!ioshape.points.empty()) {
      add_shape(scene, ioshape.points, ioshape.positions, ioshape.normals,
          ioshape.texcoords, ioshape.colors, ioshape.radius);
    } else if (!ioshape.lines.empty()) {
      add_shape(scene, ioshape.lines, ioshape.positions, ioshape.normals,
          ioshape.texcoords, ioshape.colors, ioshape.radius);
    } else if (!ioshape.triangles.empty()) {
      add_shape(scene, ioshape.triangles, ioshape.positions, ioshape.normals,
          ioshape.texcoords, ioshape.colors, ioshape.tangents);
    } else if (!ioshape.quads.empty()) {
      add_shape(scene, ioshape.quads, ioshape.positions, ioshape.normals,
          ioshape.texcoords, ioshape.colors, ioshape.tangents);
    }
    ioshape = {};
  }

  for (auto& ioinstance : ioscene.instances) {
    add_instance(
        scene, ioinstance.frame, ioinstance.shape, ioinstance.material);
  }

  for (auto& ioenvironment : ioscene.environments) {
    add_environment(scene, ioenvironment.frame, ioenvironment.emission,
        ioenvironment.emission_tex);
  }

  ioscene = {};
}

// Simple parallel for used since our target platforms do not yet support
// parallel algorithms. `Func` takes the integer index.
template <typename Func>
inline void parallel_for(const vec2i& size, Func&& func) {
  auto        futures  = vector<future<void>>{};
  auto        nthreads = thread::hardware_concurrency();
  atomic<int> next_idx(0);
  for (auto thread_id = 0; thread_id < nthreads; thread_id++) {
    futures.emplace_back(async(launch::async, [&func, &next_idx, size]() {
      while (true) {
        auto j = next_idx.fetch_add(1);
        if (j >= size.y) break;
        for (auto i = 0; i < size.x; i++) func({i, j});
      }
    }));
  }
  for (auto& f : futures) f.get();
}

// Trace a block of samples
vec4f raymarch_sample(const Csg& csg, trace_state& state,
    const trace_camera& camera, const vec2i& ij, const trace_params& params) {
  auto& pixel = state.at(ij);

  auto ray = sample_camera(
      camera, ij, state.size(), rand2f(pixel.rng), rand2f(pixel.rng));
  auto radiance = raymarch(camera, csg, ray, pixel.rng, params);

  if (!isfinite(radiance)) radiance = zero3f;
  if (max(radiance) > params.clamp)
    radiance = radiance * (params.clamp / max(radiance));
  pixel.radiance += radiance;
  pixel.hits += 1;
  pixel.samples += 1;
  return {pixel.hits ? pixel.radiance / pixel.hits : zero3f,
      (float)pixel.hits / (float)pixel.samples};
}

// Progressively compute an image by calling trace_samples multiple times.
image<vec4f> raymarch_image(
    const trace_scene& scene, const Csg& csg, const trace_params& params) {
  auto state = trace_state{};
  init_state(state, scene, params);
  auto render = image{state.size(), zero4f};

  if (params.noparallel) {
    for (auto j = 0; j < render.size().y; j++) {
      for (auto i = 0; i < render.size().x; i++) {
        for (auto s = 0; s < params.samples; s++) {
          render[{i, j}] = raymarch_sample(
              csg, state, scene.cameras[0], {i, j}, params);
        }
      }
    }
  } else {
    parallel_for(render.size(), [&render, &state, &scene, &params, &csg](
                                    const vec2i& ij) {
      for (auto s = 0; s < params.samples; s++) {
        render[ij] = raymarch_sample(csg, state, scene.cameras[0], ij, params);
      }
    });
  }

  return render;
}

void reset_display(shared_ptr<app_state> app) {
  // stop render
  app->render_stop = true;
  if (app->render_future.valid()) app->render_future.get();

  for (auto& f : app->commands) {
    f();
  }
  app->commands.clear();

  // reset state
  init_state(app->state, app->scene, app->params);
  app->render.resize(app->state.size());
  app->display.resize(app->state.size());

  // render preview
  auto preview_prms = app->params;
  preview_prms.resolution /= app->preview_downscale;
  preview_prms.samples = 1;
  auto preview         = raymarch_image(app->scene, app->csg, preview_prms);
  preview              = tonemap_image(preview, app->exposure);
  for (auto j = 0; j < app->display.size().y; j++) {
    for (auto i = 0; i < app->display.size().x; i++) {
      auto pi = clamp(i / app->preview_downscale, 0, preview.size().x - 1),
           pj = clamp(j / app->preview_downscale, 0, preview.size().y - 1);
      app->display[{i, j}] = preview[{pi, pj}];
    }
  }

  // start renderer
  app->render_counter = 0;
  app->render_stop    = false;
  app->render_future  = async(launch::async, [app]() {
    for (auto sample = 0; sample < app->params.samples; sample++) {
      if (app->render_stop) return;
      parallel_for(app->render.size(), [app](const vec2i& ij) {
        if (app->render_stop) return;
        // app->render[ij] = trace_sample(app->state, app->scene, ij,
        // app->params);
        app->render[ij] = raymarch_sample(
            app->csg, app->state, app->scene.cameras[0], ij, app->params);
        app->display[ij] = tonemap(app->render[ij], app->exposure);
      });
    }
  });
}

void run_app(const string& filename) {
  // application
  auto app      = make_shared<app_state>();
  app->csg      = load_csg(filename);
  app->filename = filename;

  // scene loading
  auto ioscene = sceneio_model{};
  // load_scene(app->filename, ioscene);
  auto camera   = sceneio_camera{};
  auto from     = vec3f{2, 2, 2};
  auto to       = vec3f{0.5, 0.5, 0.5};
  camera.aspect = 1;
  camera.frame  = lookat_frame(from, to, {0, 1, 0});
  camera.focus  = length(from - to);
  ioscene.cameras.push_back(camera);

  // conversion
  init_scene(app->scene, ioscene);

  // allocate buffers
  init_state(app->state, app->scene, app->params);
  app->render  = image{app->state.size(), zero4f};
  app->display = app->render;
  reset_display(app);

  app->params.samples = 4;

  // window
  auto win = opengl_window{};
  init_glwindow(win, {720, 720}, "yscnitraces", false);

  // callbacks
  set_draw_glcallback(
      win, [app](const opengl_window& win, const opengl_input& input) {
        if (!is_initialized(app->glimage)) init_glimage(app->glimage);
        if (!app->render_counter)
          set_glimage(app->glimage, app->display, false, false);
        app->glparams.window      = input.window_size;
        app->glparams.framebuffer = input.framebuffer_viewport;
        update_imview(app->glparams.center, app->glparams.scale,
            app->display.size(), app->glparams.window, app->glparams.fit);
        draw_glimage(app->glimage, app->glparams);
        app->render_counter++;
        if (app->render_counter > 10) app->render_counter = 0;
      });
  set_uiupdate_glcallback(
      win, [app](const opengl_window& win, const opengl_input& input) {
        if ((input.mouse_left || input.mouse_right) && !input.modifier_alt) {
          auto& camera = app->scene.cameras.at(app->params.camera);
          auto  dolly  = 0.0f;
          auto  pan    = zero2f;
          auto  rotate = zero2f;
          if (input.mouse_left && !input.modifier_shift)
            rotate = (input.mouse_pos - input.mouse_last) / 100.0f;
          if (input.mouse_right)
            dolly = (input.mouse_pos.x - input.mouse_last.x) / 100.0f;
          if (input.mouse_left && input.modifier_shift)
            pan = (input.mouse_pos - input.mouse_last) * camera.focus / 200.0f;
          pan.x = -pan.x;
          update_turntable(camera.frame, camera.focus, rotate, dolly, pan);
          reset_display(app);
        }
      });

  auto keycb = [app](const opengl_window& win, opengl_key key, bool pressed,
                   const opengl_input& input) {
    if (key == opengl_key::enter && pressed) {
      app->run([app]() { app->csg = load_csg(app->filename); });
      reset_display(app);
    }
  };

  set_key_glcallback(win, keycb);

  // run ui
  run_ui(win);

  // clear
  clear_glwindow(win);
}

int main(int argc, const char* argv[]) {
  string filename;
  // parse command line
  auto cli = make_cli("michelangelo", "Csg renderer");
  add_cli_option(cli, "scene", filename, "Scene filename", true);
  parse_cli(cli, argc, argv);

  run_app(filename);
}
