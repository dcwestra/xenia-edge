/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/kernel/xam/avatar_assets.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

struct Vec3 {
  float x, y, z;
};

// A rigid transform: v' = rotation * v + translation.
struct Transform {
  float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  float t[3] = {0, 0, 0};
  Vec3 Apply(const Vec3& v) const {
    return {r[0] * v.x + r[1] * v.y + r[2] * v.z + t[0],
            r[3] * v.x + r[4] * v.y + r[5] * v.z + t[1],
            r[6] * v.x + r[7] * v.y + r[8] * v.z + t[2]};
  }
  Vec3 Rotate(const Vec3& v) const {
    return {r[0] * v.x + r[1] * v.y + r[2] * v.z,
            r[3] * v.x + r[4] * v.y + r[5] * v.z,
            r[6] * v.x + r[7] * v.y + r[8] * v.z};
  }
  // this * other.
  Transform Then(const Transform& other) const {
    Transform out;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        out.r[3 * i + j] = r[3 * i] * other.r[j] +
                           r[3 * i + 1] * other.r[3 + j] +
                           r[3 * i + 2] * other.r[6 + j];
      }
      out.t[i] = r[3 * i] * other.t[0] + r[3 * i + 1] * other.t[1] +
                 r[3 * i + 2] * other.t[2] + t[i];
    }
    return out;
  }
};

// A rotation by angle radians about the z axis through a point.
Transform RotationZAbout(const float* point, float angle) {
  Transform rotation;
  const float c = std::cos(angle), s = std::sin(angle);
  rotation.r[0] = c;
  rotation.r[1] = -s;
  rotation.r[3] = s;
  rotation.r[4] = c;
  const Vec3 p = {point[0], point[1], point[2]};
  const Vec3 rp = rotation.Rotate(p);
  rotation.t[0] = p.x - rp.x;
  rotation.t[1] = p.y - rp.y;
  rotation.t[2] = p.z - rp.z;
  return rotation;
}

// The bones' transforms from the bind pose (a T-pose) to an idle one with the
// arms down: the shoulders (bones 20 and 22) turned down, their arms with
// them.
std::array<Transform, 71> IdlePose() {
  constexpr float kArmDown = 1.2f;
  std::array<Transform, 71> pose;
  std::array<bool, 71> done = {};
  for (size_t i = 0; i < pose.size(); ++i) {
    // Parents first.
    std::vector<size_t> chain;
    for (size_t b = i; b < pose.size() && !done[b];
         b = kAvatarSkeleton[b].parent) {
      chain.push_back(b);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const AvatarBone& bone = kAvatarSkeleton[*it];
      Transform local;
      if (*it == 20 || *it == 22) {
        local =
            RotationZAbout(bone.model_position,
                           bone.model_position[0] > 0 ? -kArmDown : kArmDown);
      }
      pose[*it] =
          bone.parent < pose.size() ? pose[bone.parent].Then(local) : local;
      done[*it] = true;
    }
  }
  return pose;
}

float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t(half >> 15) << 31;
  const uint32_t exponent = (half >> 10) & 0x1F;
  uint32_t mantissa = half & 0x3FF;
  uint32_t bits;
  if (!exponent) {
    if (!mantissa) {
      bits = sign;
    } else {
      // Subnormal.
      int32_t e = -1;
      do {
        ++e;
        mantissa <<= 1;
      } while (!(mantissa & 0x400));
      bits = sign | uint32_t(127 - 15 - e) << 23 | (mantissa & 0x3FF) << 13;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000 | mantissa << 13;
  } else {
    bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

float SignedBits(uint32_t value, uint32_t shift, uint32_t width) {
  int32_t v = int32_t((value >> shift) & ((1u << width) - 1));
  if (v & (1 << (width - 1))) {
    v -= 1 << width;
  }
  return float(v) / float((1 << (width - 1)) - 1);
}

struct Texture {
  uint32_t width = 0, height = 0;
  std::vector<uint32_t> texels;
  // RGBA as 0-1, wrapped, nearest.
  void Sample(float u, float v, float* out) const {
    if (texels.empty()) {
      out[0] = out[1] = out[2] = out[3] = 0.0f;
      return;
    }
    u -= std::floor(u);
    v -= std::floor(v);
    const uint32_t x = std::min(uint32_t(u * width), width - 1);
    const uint32_t y = std::min(uint32_t(v * height), height - 1);
    const uint32_t c = texels[size_t(y) * width + x];
    for (int i = 0; i < 4; ++i) {
      out[i] = float((c >> (8 * i)) & 0xFF) / 255.0f;
    }
  }
};

struct Vertex {
  Vec3 position;
  Vec3 normal;
  float occlusion;
  float uv[6][2];
};

void Lerp(float* color, const float* layer, float amount) {
  for (int i = 0; i < 3; ++i) {
    color[i] += (layer[i] - color[i]) * amount;
  }
}

}  // namespace

bool RenderAvatarPreview(const AvatarAssets& assets, uint32_t width,
                         uint32_t height, float yaw,
                         std::vector<uint8_t>* rgb) {
  rgb->assign(size_t(width) * height * 3, 0);
  // A soft vertical gradient behind it.
  for (uint32_t y = 0; y < height; ++y) {
    const float t = float(y) / float(std::max<uint32_t>(height - 1, 1));
    const uint8_t shade[3] = {uint8_t(206 - 50 * t), uint8_t(214 - 50 * t),
                              uint8_t(226 - 50 * t)};
    for (uint32_t x = 0; x < width; ++x) {
      std::memcpy(&(*rgb)[(size_t(y) * width + x) * 3], shade, 3);
    }
  }
  if (assets.models.empty() || !width || !height) {
    return false;
  }
  const std::array<Transform, 71> pose = IdlePose();
  const float cos_yaw = std::cos(yaw), sin_yaw = std::sin(yaw);
  struct Mesh {
    const AvatarAssetModel* model;
    uint32_t pass;
    // The body a little behind the clothes over it, which it pokes through
    // in places.
    float depth_bias;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    // Texture binding n samples UV set n.
    std::vector<const Texture*> textures;
    std::map<uint32_t, std::array<float, 3>> colors;
  };
  std::vector<Mesh> meshes;
  std::vector<std::vector<Texture>> textures(assets.models.size());
  const auto& gpu = assets.gpu_data;
  auto load32 = [&gpu](size_t offset) {
    return offset + 4 <= gpu.size() ? xe::load_and_swap<uint32_t>(&gpu[offset])
                                    : 0u;
  };
  float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
  for (size_t m = 0; m < assets.models.size(); ++m) {
    const AvatarAssetModel& model = assets.models[m];
    textures[m].resize(model.textures.size());
    for (size_t t = 0; t < model.textures.size(); ++t) {
      Texture& texture = textures[m][t];
      if (DecodeAvatarTexture(assets, model.textures[t], 0, &texture.texels)) {
        texture.width = model.textures[t][1];
        texture.height = model.textures[t][2];
      }
    }
    for (const auto& batch : model.batches) {
      auto word = [&batch](size_t i) {
        return xe::load_and_swap<uint32_t>(&batch[4 * i]);
      };
      Mesh mesh;
      mesh.model = &model;
      mesh.pass = word(0);
      mesh.depth_bias = (model.slots & 0x2) ? -0.006f : 0.0f;
      for (size_t r = 1; r + 6 <= 120; r += 6) {
        const uint32_t type = word(r);
        if (type == 1) {
          const uint32_t index = word(r + 2) & 0xFFFF;
          mesh.textures.push_back(
              index < textures[m].size() ? &textures[m][index] : nullptr);
        } else if (type == 3) {
          std::array<float, 3> color;
          for (int i = 0; i < 3; ++i) {
            const uint32_t bits = word(r + 2 + i);
            std::memcpy(&color[i], &bits, sizeof(float));
          }
          mesh.colors[word(r + 1)] = color;
        } else {
          break;
        }
      }
      const uint32_t index_count = word(121) * 3;
      const uint32_t vertex_count = word(122);
      const uint32_t uv_sets = std::min<uint32_t>(word(123), 6);
      const uint32_t stride = word(124);
      const size_t vb = word(126), ib = word(127);
      if (stride < 28 + 4 * uv_sets ||
          vb + size_t(vertex_count) * stride > gpu.size() ||
          ib + size_t(index_count) * 2 > gpu.size()) {
        continue;
      }
      mesh.vertices.resize(vertex_count);
      for (uint32_t v = 0; v < vertex_count; ++v) {
        const size_t base = vb + size_t(v) * stride;
        Vec3 position;
        const uint32_t px = load32(base), py = load32(base + 4),
                       pz = load32(base + 8);
        std::memcpy(&position.x, &px, 4);
        std::memcpy(&position.y, &py, 4);
        std::memcpy(&position.z, &pz, 4);
        const uint32_t packed_normal = load32(base + 12);
        const Vec3 normal = {SignedBits(packed_normal, 0, 11),
                             SignedBits(packed_normal, 11, 11),
                             SignedBits(packed_normal, 22, 10)};
        // Skinned by the bone indices and weights (byte n of each word).
        const uint32_t weights = load32(base + 16);
        const uint32_t bones = load32(base + 20);
        Vec3 p = {0, 0, 0}, n = {0, 0, 0};
        float total = 0.0f;
        for (uint32_t k = 0; k < 4; ++k) {
          const float w = float((weights >> (8 * k)) & 0xFF) / 255.0f;
          const uint32_t bone = (bones >> (8 * k)) & 0xFF;
          if (w <= 0.0f || bone >= pose.size()) {
            continue;
          }
          const Vec3 bp = pose[bone].Apply(position);
          const Vec3 bn = pose[bone].Rotate(normal);
          p = {p.x + bp.x * w, p.y + bp.y * w, p.z + bp.z * w};
          n = {n.x + bn.x * w, n.y + bn.y * w, n.z + bn.z * w};
          total += w;
        }
        if (total <= 0.0f) {
          p = position;
          n = normal;
        } else {
          p = {p.x / total, p.y / total, p.z / total};
        }
        Vertex& out = mesh.vertices[v];
        // Turned about the vertical axis.
        out.position = {p.x * cos_yaw + p.z * sin_yaw, p.y,
                        -p.x * sin_yaw + p.z * cos_yaw};
        const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        const float inv = length > 0.0f ? 1.0f / length : 0.0f;
        out.normal = {(n.x * cos_yaw + n.z * sin_yaw) * inv, n.y * inv,
                      (-n.x * sin_yaw + n.z * cos_yaw) * inv};
        out.occlusion = float(load32(base + 24) & 0xFF) / 255.0f;
        for (uint32_t s = 0; s < uv_sets; ++s) {
          out.uv[s][0] =
              HalfToFloat(xe::load_and_swap<uint16_t>(&gpu[base + 28 + 4 * s]));
          out.uv[s][1] =
              HalfToFloat(xe::load_and_swap<uint16_t>(&gpu[base + 30 + 4 * s]));
        }
        min_x = std::min(min_x, out.position.x);
        max_x = std::max(max_x, out.position.x);
        min_y = std::min(min_y, out.position.y);
        max_y = std::max(max_y, out.position.y);
      }
      mesh.indices.resize(index_count);
      for (uint32_t i = 0; i < index_count; ++i) {
        mesh.indices[i] = xe::load_and_swap<uint16_t>(&gpu[ib + 2 * size_t(i)]);
      }
      meshes.push_back(std::move(mesh));
    }
  }
  if (meshes.empty() || max_x <= min_x || max_y <= min_y) {
    return false;
  }
  // Orthographic, fitted with a margin.
  const float margin = 0.06f * float(std::min(width, height));
  const float scale = std::min((width - 2 * margin) / (max_x - min_x),
                               (height - 2 * margin) / (max_y - min_y));
  const float center_x = 0.5f * (min_x + max_x);
  const float center_y = 0.5f * (min_y + max_y);
  std::vector<float> depth(size_t(width) * height, -1e9f);
  const Vec3 light = {0.35f, 0.55f, 0.76f};
  for (const Mesh& mesh : meshes) {
    auto color_of = [&mesh](uint32_t id, float* out) {
      auto it = mesh.colors.find(id);
      if (it == mesh.colors.end()) {
        out[0] = out[1] = out[2] = 1.0f;
        return;
      }
      std::copy(it->second.begin(), it->second.end(), out);
    };
    float tints[3][3];
    for (uint32_t i = 0; i < 3; ++i) {
      color_of(0x16 + i, tints[i]);
    }
    float head_colors[9][3];
    for (uint32_t i = 0; i < 9; ++i) {
      color_of(0xD + i, head_colors[i]);
    }
    auto texture = [&mesh](size_t n) -> const Texture* {
      return n < mesh.textures.size() && mesh.textures[n] &&
                     !mesh.textures[n]->texels.empty()
                 ? mesh.textures[n]
                 : nullptr;
    };
    for (size_t i = 0; i + 3 <= mesh.indices.size(); i += 3) {
      const uint32_t a = mesh.indices[i], b = mesh.indices[i + 1],
                     c = mesh.indices[i + 2];
      if (a >= mesh.vertices.size() || b >= mesh.vertices.size() ||
          c >= mesh.vertices.size()) {
        continue;
      }
      const Vertex* v[3] = {&mesh.vertices[a], &mesh.vertices[b],
                            &mesh.vertices[c]};
      float screen_x[3], screen_y[3];
      for (int k = 0; k < 3; ++k) {
        screen_x[k] = 0.5f * width + (v[k]->position.x - center_x) * scale;
        screen_y[k] = 0.5f * height - (v[k]->position.y - center_y) * scale;
      }
      const float area =
          (screen_x[1] - screen_x[0]) * (screen_y[2] - screen_y[0]) -
          (screen_x[2] - screen_x[0]) * (screen_y[1] - screen_y[0]);
      if (std::fabs(area) < 1e-6f) {
        continue;
      }
      const int x0 = std::max(
          0,
          int(std::floor(std::min({screen_x[0], screen_x[1], screen_x[2]}))));
      const int x1 = std::min(
          int(width) - 1,
          int(std::ceil(std::max({screen_x[0], screen_x[1], screen_x[2]}))));
      const int y0 = std::max(
          0,
          int(std::floor(std::min({screen_y[0], screen_y[1], screen_y[2]}))));
      const int y1 = std::min(
          int(height) - 1,
          int(std::ceil(std::max({screen_y[0], screen_y[1], screen_y[2]}))));
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const float px = x + 0.5f, py = y + 0.5f;
          float w[3] = {((screen_x[1] - px) * (screen_y[2] - py) -
                         (screen_x[2] - px) * (screen_y[1] - py)) /
                            area,
                        ((screen_x[2] - px) * (screen_y[0] - py) -
                         (screen_x[0] - px) * (screen_y[2] - py)) /
                            area,
                        0.0f};
          w[2] = 1.0f - w[0] - w[1];
          if (w[0] < 0.0f || w[1] < 0.0f || w[2] < 0.0f) {
            continue;
          }
          const float z = w[0] * v[0]->position.z + w[1] * v[1]->position.z +
                          w[2] * v[2]->position.z + mesh.depth_bias;
          float& stored = depth[size_t(y) * width + x];
          if (z <= stored) {
            continue;
          }
          stored = z;
          auto uv = [&](size_t set, float* out) {
            const size_t s = std::min<size_t>(set, 5);
            out[0] = w[0] * v[0]->uv[s][0] + w[1] * v[1]->uv[s][0] +
                     w[2] * v[2]->uv[s][0];
            out[1] = w[0] * v[0]->uv[s][1] + w[1] * v[1]->uv[s][1] +
                     w[2] * v[2]->uv[s][1];
          };
          float color[3] = {1.0f, 1.0f, 1.0f};
          float sample[4], coords[2];
          if (mesh.pass == 4) {
            // The head: skin, then face paint, eye shadow, mouth, eyes,
            // facial hair and eyebrows, each its texture's alpha over the
            // rest; a mask's red picks its color, green white, blue skin.
            std::copy_n(head_colors[0], 3, color);
            auto layer = [&](size_t n, const float* red, const float* green,
                             bool red_only) {
              const Texture* t = texture(n);
              if (!t) {
                return;
              }
              uv(n, coords);
              t->Sample(coords[0], coords[1], sample);
              float mixed[3];
              for (int k = 0; k < 3; ++k) {
                mixed[k] = red_only
                               ? red[k] * sample[0]
                               : red[k] * sample[0] + green[k] * sample[1] +
                                     head_colors[0][k] * sample[2];
              }
              Lerp(color, mixed, sample[3]);
            };
            static const float kWhite[3] = {1.0f, 1.0f, 1.0f};
            layer(0, head_colors[7], head_colors[8], false);
            layer(5, head_colors[5], kWhite, true);
            layer(4, head_colors[2], kWhite, false);
            layer(3, head_colors[3], kWhite, false);
            layer(1, head_colors[6], kWhite, false);
            layer(2, head_colors[4], kWhite, false);
          } else {
            // Base, then the tint mask's colors by its alpha, then the
            // overlay by its alpha.
            if (const Texture* t = texture(0)) {
              uv(0, coords);
              t->Sample(coords[0], coords[1], sample);
              std::copy_n(sample, 3, color);
            }
            if (const Texture* t = texture(1)) {
              uv(1, coords);
              t->Sample(coords[0], coords[1], sample);
              float tint[3];
              for (int k = 0; k < 3; ++k) {
                tint[k] = tints[0][k] * sample[0] + tints[1][k] * sample[1] +
                          tints[2][k] * sample[2];
              }
              Lerp(color, tint, sample[3]);
            }
            if (const Texture* t = texture(2)) {
              uv(2, coords);
              t->Sample(coords[0], coords[1], sample);
              Lerp(color, sample, sample[3]);
            }
          }
          Vec3 n = {w[0] * v[0]->normal.x + w[1] * v[1]->normal.x +
                        w[2] * v[2]->normal.x,
                    w[0] * v[0]->normal.y + w[1] * v[1]->normal.y +
                        w[2] * v[2]->normal.y,
                    w[0] * v[0]->normal.z + w[1] * v[1]->normal.z +
                        w[2] * v[2]->normal.z};
          const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
          const float lambert =
              length > 0.0f
                  ? std::max(0.0f,
                             (n.x * light.x + n.y * light.y + n.z * light.z) /
                                 length)
                  : 0.5f;
          const float occlusion = w[0] * v[0]->occlusion +
                                  w[1] * v[1]->occlusion +
                                  w[2] * v[2]->occlusion;
          const float shade = (0.45f + 0.6f * lambert) * occlusion;
          uint8_t* out = &(*rgb)[(size_t(y) * width + x) * 3];
          for (int k = 0; k < 3; ++k) {
            out[k] = uint8_t(std::clamp(color[k] * shade, 0.0f, 1.0f) * 255.0f);
          }
        }
      }
    }
  }
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
