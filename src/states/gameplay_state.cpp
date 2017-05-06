// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "states/gameplay_state.h"

#include "mathfu/internal/disable_warnings_begin.h"

#include "mathfu/internal/disable_warnings_end.h"

#include "fplbase/asset_manager.h"
#include "fplbase/input.h"
#include "full_screen_fader.h"
#include "game.h"
#include "input_config_generated.h"
#include "mathfu/glsl_mappings.h"
#include "states/states.h"
#include "states/states_common.h"
#include "world.h"

// In windows.h, PlaySound is #defined to be either PlaySoundW or PlaySoundA.
// We need to undef this macro or AudioEngine::PlaySound() won't compile.
// TODO(amablue): Change our PlaySound to have a different name (b/30090037).
#if defined(PlaySound)
#undef PlaySound
#endif  // defined(PlaySound)

using mathfu::vec2i;
using mathfu::vec2;

namespace fpl {
namespace zooshi {

// Update music gain based on lap number. This logic will eventually live in
// an event graph.
static void UpdateMusic(corgi::EntityManager* entity_manager, int* previous_lap,
                        float* percent, int delta_time,
                        pindrop::Channel* music_channel_1,
                        pindrop::Channel* music_channel_2,
                        pindrop::Channel* music_channel_3) {
  corgi::EntityRef raft =
      entity_manager->GetComponent<ServicesComponent>()->raft_entity();
  RailDenizenData* raft_rail_denizen =
      entity_manager->GetComponentData<RailDenizenData>(raft);
  if (raft_rail_denizen == nullptr) return;
  int current_lap = raft_rail_denizen->lap_number;
  assert(current_lap >= 0);
  if (current_lap != *previous_lap) {
    pindrop::Channel* channels[] = {music_channel_1, music_channel_2,
                                    music_channel_3};
    const float kCrossFadeDuration = 5.0f;
    bool done = false;
    float seconds = delta_time / 1000.0f;
    float delta = seconds / kCrossFadeDuration;
    pindrop::Channel* channel_previous = channels[*previous_lap % 3];
    pindrop::Channel* channel_current = channels[current_lap % 3];
    *percent += delta;
    if (*percent >= 1.0f) {
      *percent = 1.0f;
      done = true;
    }
    // Equal power crossfade
    //    https://www.safaribooksonline.com/library/view/web-audio-api/9781449332679/s03_2.html
    // TODO: Add utility functions to Pindrop for this.
    float gain_previous = cos(*percent * 0.5f * static_cast<float>(M_PI));
    float gain_current =
        cos((1.0f - *percent) * 0.5f * static_cast<float>(M_PI));
    channel_previous->SetGain(gain_previous);
    channel_current->SetGain(gain_current);

    if (done) {
      *previous_lap = current_lap;
      *percent = 0.0f;
    }
  }
}

void GameplayState::AdvanceFrame(int delta_time, int* next_state) {
  // Update the world.
  world_->entity_manager.UpdateComponents(delta_time);
  UpdateMainCamera(&main_camera_, world_);
  UpdateMusic(&world_->entity_manager, &previous_lap_, &percent_, delta_time,
              &music_channel_lap_1_, &music_channel_lap_2_,
              &music_channel_lap_3_);

  if (input_system_->GetButton(fplbase::FPLK_F9).went_down()) {
    world_->draw_debug_physics = !world_->draw_debug_physics;
  }
  if (input_system_->GetButton(fplbase::FPLK_F8).went_down()) {
    world_->skip_rendermesh_rendering = !world_->skip_rendermesh_rendering;
  }

  // The state machine for the world may request a state change.
  *next_state = requested_state_;

  // Switch into scene lab if the keyboard requests.
  // Switch back to scene lab if we're single stepping.
  if (scene_lab_ && (input_system_->GetButton(fplbase::FPLK_F10).went_down() ||
                     input_system_->GetButton(fplbase::FPLK_1).went_down() ||
                     world_->is_single_stepping)) {
    if (!world_->is_single_stepping) {
      scene_lab::GenericCamera camera;
      camera.position = main_camera_.position();
      camera.facing = main_camera_.facing();
      camera.up = main_camera_.up();
      scene_lab_->SetInitialCamera(camera);
    }
    *next_state = kGameStateSceneLab;
    world_->is_single_stepping = false;
  }

  // Pause the game.
  if (input_system_->GetButton(fplbase::FPLK_ESCAPE).went_down() ||
      input_system_->GetButton(fplbase::FPLK_AC_BACK).went_down()) {
    audio_engine_->PlaySound(sound_pause_);
    *next_state = kGameStatePause;
  }
  fader_->AdvanceFrame(delta_time);
}

void GameplayState::RenderPrep() {
  world_->world_renderer->RenderPrep(main_camera_, world_);
}

void GameplayState::Render(fplbase::Renderer* renderer) {
  if (!world_->asset_manager) return;
  Camera* cardboard_camera = nullptr;
#if FPLBASE_ANDROID_VR
  cardboard_camera = &cardboard_camera_;
#endif
  RenderWorld(*renderer, world_, main_camera_, cardboard_camera, input_system_);
  if (!fader_->Finished()) {
    renderer->set_model_view_projection(
        mathfu::mat4::Ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f));
    fader_->Render(renderer);
  }
}

void GameplayState::HandleUI(fplbase::Renderer* renderer) {
  ServicesComponent& services = world_->services_component;
  world_->onscreen_controller_ui.Update(services.asset_manager(),
                                        services.font_manager(),
                                        renderer->window_size());
}

void GameplayState::Initialize(
    fplbase::InputSystem* input_system, World* world, const Config* config,
    const InputConfig* input_config, corgi::EntityManager* entity_manager,
    scene_lab::SceneLab* scene_lab, GPGManager* gpg_manager,
    pindrop::AudioEngine* audio_engine, FullScreenFader* fader) {
  input_system_ = input_system;
  config_ = config;
  world_ = world;
  input_config_ = input_config;
  entity_manager_ = entity_manager;
  scene_lab_ = scene_lab;
  gpg_manager_ = gpg_manager;
  audio_engine_ = audio_engine;
  fader_ = fader;

  sound_pause_ = audio_engine->GetSoundHandle("pause");
  music_gameplay_lap_1_ = audio_engine->GetSoundHandle("music_gameplay_lap_1");
  music_gameplay_lap_2_ = audio_engine->GetSoundHandle("music_gameplay_lap_2");
  music_gameplay_lap_3_ = audio_engine->GetSoundHandle("music_gameplay_lap_3");

#if FPLBASE_ANDROID_VR
  cardboard_camera_.set_viewport_angle(config->cardboard_viewport_angle());
#else
  (void)config;
#endif
}

void GameplayState::OnEnter(int previous_state) {
  requested_state_ = kGameStateGameplay;
  world_->player_component.set_state(kPlayerState_Active);
  input_system_->SetRelativeMouseMode(true);
  UpdateMainCamera(&main_camera_, world_);

  // Assign textures for the onscreen controller.
  auto* onscreen_controller_ui = &world_->onscreen_controller_ui;
  auto* asset_manager = world_->asset_manager;
  onscreen_controller_ui->set_base_texture(
      asset_manager->FindTexture("textures/joystick_base.webp"));
  onscreen_controller_ui->set_top_texture(
      asset_manager->FindTexture("textures/joystick_tip.webp"));

  if (previous_state == kGameStatePause) {
    music_channel_lap_1_.Resume();
    music_channel_lap_2_.Resume();
    music_channel_lap_3_.Resume();
  } else {
    music_channel_lap_1_ =
        audio_engine_->PlaySound(music_gameplay_lap_1_, mathfu::kZeros3f, 1.0f);
    music_channel_lap_2_ =
        audio_engine_->PlaySound(music_gameplay_lap_2_, mathfu::kZeros3f, 0.0f);
    music_channel_lap_3_ =
        audio_engine_->PlaySound(music_gameplay_lap_3_, mathfu::kZeros3f, 0.0f);
  }

  if (world_->rendering_mode() == kRenderingStereoscopic) {
#if FPLBASE_ANDROID_VR
    world_->services_component.set_camera(&cardboard_camera_);
#endif
  } else {
    world_->services_component.set_camera(&main_camera_);
  }
#if FPLBASE_ANDROID_VR
  input_system_->head_mounted_display_input().ResetHeadTracker();
#endif  // FPLBASE_ANDROID_VR

  // Perform Analytics
  if (previous_state != kGameStatePause) {
    // Set the start time, so elapsed time can be tracked.
    world_->gameplay_start_time = input_system_->Time();
  }
}

void GameplayState::OnExit(int next_state) {
  if (next_state == kGameStatePause) {
    music_channel_lap_1_.Pause();
    music_channel_lap_2_.Pause();
    music_channel_lap_3_.Pause();
  } else {
    music_channel_lap_1_.Stop();
    music_channel_lap_2_.Stop();
    music_channel_lap_3_.Stop();
  }
}

}  // zooshi
}  // fpl
