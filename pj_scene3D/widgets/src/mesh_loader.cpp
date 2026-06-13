// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "mesh_loader.h"

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <QDir>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <array>
#include <assimp/Importer.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <vector>

namespace pj::scene3d {
namespace {

// Post-process flags shared by both entry points (design §B.4):
//   Triangulate          — render passes draw triangles only.
//   GenSmoothNormals      — generate normals only for meshes that arrive WITHOUT
//                           any (assimp skips meshes that already carry normals,
//                           e.g. STL/OBJ, leaving their normals untouched).
//   JoinIdenticalVertices — index the soup so we get a real index buffer.
//   FlipUVs               — match the GL texture origin convention.
//   ImproveCacheLocality  — reorder indices for the post-T&L vertex cache
//                           (added in lieu of meshoptimizer).
//   CalcTangentSpace      — generate per-vertex tangents/bitangents for normal
//                           mapping (only for meshes that carry UVs + normals).
// Deliberately NOT aiProcess_PreTransformVertices: it bakes the scene graph into
// world space. We keep the node hierarchy so appendNode() can apply each node's
// transform itself — including the root transform assimp's Collada importer uses
// to normalize the file's <up_axis> to assimp's internal Y-up convention.
constexpr unsigned int kPostProcessFlags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                                           aiProcess_JoinIdenticalVertices | aiProcess_FlipUVs |
                                           aiProcess_ImproveCacheLocality | aiProcess_CalcTangentSpace;

// Explicit Y-up -> Z-up rotation (+90deg about X): (x, y, z) -> (x, -z, y).
// Applied to formats that assimp delivers in Y-up — glTF (Y-up by spec) AND
// Collada, which assimp's importer normalizes from the file's <up_axis> to its
// internal Y-up. Verified empirically (zup_marker.dae fixture): a Z_UP-authored
// apex at (0,0,1) arrives as (0,1,0) and is restored to (0,0,1) by this flip.
glm::mat3 zUpFromYUpRotation() {
  // R_x(+90deg): (x, y, z) -> (x, -z, y).
  return glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
}

// FNV-1a over a byte span — a cheap, stable content hash for embedded-texture
// cache keys (dedups identical embedded images and survives GL-context recreation).
std::string contentHashKey(const std::uint8_t* data, std::size_t size) {
  std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
  for (std::size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;  // FNV prime
  }
  std::array<char, 17> buf{};
  std::snprintf(buf.data(), buf.size(), "%016llx", static_cast<unsigned long long>(h));
  return std::string("emb:") + buf.data();
}

// Resolve one texture map of a material into a TextureSource. Embedded glTF/GLB
// images (assimp ref "*N") become inline bytes keyed by content hash; external
// refs become an absolute filesystem path (resolved relative to base_dir, with a
// small extension-retry for refs that drop their suffix). Only compressed
// embedded images (PNG/JPEG, the universal glTF case where aiTexture::mHeight ==
// 0) are supported; raw embedded RGBA (mHeight > 0, rare) is skipped and the
// slot stays empty.
TextureSource resolveTexture(const aiScene* scene, const aiMaterial* mat, aiTextureType type, const QString& base_dir) {
  aiString texture_path;
  if (aiGetMaterialTexture(mat, type, 0, &texture_path) != AI_SUCCESS) {
    return {};
  }
  const QString raw = QString::fromUtf8(texture_path.C_Str());
  if (raw.isEmpty()) {
    return {};
  }

  if (raw.startsWith(QLatin1Char('*'))) {
    const aiTexture* tex = scene->GetEmbeddedTexture(texture_path.C_Str());
    if (tex == nullptr || tex->mHeight != 0 || tex->pcData == nullptr || tex->mWidth == 0) {
      return {};  // missing, or raw-RGBA embedded (unsupported) — leave untextured.
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(tex->pcData);
    TextureSource source;
    source.bytes.assign(bytes, bytes + tex->mWidth);  // mWidth is the encoded byte count when mHeight == 0.
    source.key = contentHashKey(bytes, tex->mWidth);
    return source;
  }

  const QString ref = QDir::fromNativeSeparators(raw);
  const auto resolve = [&base_dir](const QString& path) -> QString {
    QString candidate;
    if (QDir::isAbsolutePath(path)) {
      candidate = path;
    } else if (!base_dir.isEmpty()) {
      candidate = QDir(base_dir).absoluteFilePath(path);
    } else {
      return {};
    }
    const QFileInfo info(QDir::cleanPath(candidate));
    return info.isFile() ? info.absoluteFilePath() : QString{};
  };

  QString resolved = resolve(ref);
  if (resolved.isEmpty()) {
    static constexpr const char* kTextureExtensions[] = {".jpg", ".png", ".jpeg", ".tga", ".bmp"};
    for (const char* extension : kTextureExtensions) {
      resolved = resolve(ref + QLatin1String(extension));
      if (!resolved.isEmpty()) {
        break;
      }
    }
  }
  if (resolved.isEmpty()) {
    return {};
  }
  TextureSource source;
  source.path = resolved;
  source.key = resolved.toStdString();
  return source;
}

// Read a full metallic-roughness material (factors + five texture maps) from one
// assimp material. `has_pbr` records whether the source actually specified PBR
// factors, so the renderer can fall back to the scene-wide shading defaults for
// materials that carry none (STL, untextured primitives).
Material readMaterial(const aiScene* scene, unsigned int material_index, const QString& base_dir) {
  Material out;
  if (!scene->HasMaterials() || material_index >= scene->mNumMaterials) {
    return out;
  }
  const aiMaterial* mat = scene->mMaterials[material_index];

  aiColor4D base;
  if (mat->Get(AI_MATKEY_BASE_COLOR, base) == AI_SUCCESS) {
    out.base_color_factor = glm::vec4(base.r, base.g, base.b, base.a);
  } else {
    aiColor4D diffuse;
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
      out.base_color_factor = glm::vec4(diffuse.r, diffuse.g, diffuse.b, diffuse.a);
    }
  }

  float metallic = 0.0f;
  float roughness = 1.0f;
  if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
    out.metallic_factor = metallic;
    out.has_pbr = true;
  }
  if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
    out.roughness_factor = roughness;
    out.has_pbr = true;
  }

  aiColor3D emissive;
  if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
    out.emissive_factor = glm::vec3(emissive.r, emissive.g, emissive.b);
  }
  float emissive_intensity = 1.0f;
  if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissive_intensity) == AI_SUCCESS) {
    out.emissive_factor *= emissive_intensity;
  }

  aiString alpha_mode;
  if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) == AI_SUCCESS) {
    const QString mode = QString::fromUtf8(alpha_mode.C_Str());
    if (mode == QLatin1String("MASK")) {
      out.alpha_mode = AlphaMode::kMask;
    } else if (mode == QLatin1String("BLEND")) {
      out.alpha_mode = AlphaMode::kBlend;
    }
  }
  float cutoff = 0.5f;
  if (mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff) == AI_SUCCESS) {
    out.alpha_cutoff = cutoff;
  }

  // glTF doubleSided (and whatever other formats assimp maps to TWOSIDED): when
  // back-face culling is enabled, the renderer drops it for these submeshes
  // (see Material::double_sided in mesh_data.h).
  int two_sided = 0;
  if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS) {
    out.double_sided = two_sided != 0;
  }

  out.base_color = resolveTexture(scene, mat, aiTextureType_BASE_COLOR, base_dir);
  if (out.base_color.empty()) {
    out.base_color = resolveTexture(scene, mat, aiTextureType_DIFFUSE, base_dir);
  }
  // assimp's glTF2 importer exposes the packed metallic-roughness image under the
  // metalness slot (and aiTextureType_UNKNOWN on older versions) — same file, the
  // shader reads G=roughness/B=metallic.
  out.metallic_roughness = resolveTexture(scene, mat, aiTextureType_METALNESS, base_dir);
  if (out.metallic_roughness.empty()) {
    out.metallic_roughness = resolveTexture(scene, mat, aiTextureType_DIFFUSE_ROUGHNESS, base_dir);
  }
  if (out.metallic_roughness.empty()) {
    out.metallic_roughness = resolveTexture(scene, mat, aiTextureType_UNKNOWN, base_dir);
  }
  out.normal = resolveTexture(scene, mat, aiTextureType_NORMALS, base_dir);
  out.occlusion = resolveTexture(scene, mat, aiTextureType_AMBIENT_OCCLUSION, base_dir);
  if (out.occlusion.empty()) {
    out.occlusion = resolveTexture(scene, mat, aiTextureType_LIGHTMAP, base_dir);
  }
  out.emissive = resolveTexture(scene, mat, aiTextureType_EMISSIVE, base_dir);
  return out;
}

// Walk the node graph, accumulating each mesh's transformed vertices/indices.
// `node_transform` is the cumulative parent transform (column-major glm).
void appendNode(
    const aiScene* scene, const aiNode* node, const glm::mat4& parent_transform, const glm::mat3& extra_rotation,
    const std::vector<std::shared_ptr<const Material>>& materials, MeshData& out) {
  // assimp row-major aiMatrix4x4 -> glm column-major.
  const aiMatrix4x4& m = node->mTransformation;
  glm::mat4 local(
      m.a1, m.b1, m.c1, m.d1,   // col 0
      m.a2, m.b2, m.c2, m.d2,   // col 1
      m.a3, m.b3, m.c3, m.d3,   // col 2
      m.a4, m.b4, m.c4, m.d4);  // col 3
  const glm::mat4 node_transform = parent_transform * local;
  const glm::mat3 normal_matrix = glm::mat3(glm::transpose(glm::inverse(node_transform)));
  const glm::mat3 linear = glm::mat3(node_transform);

  for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
    const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
    static const std::shared_ptr<const Material> kFallback = std::make_shared<const Material>();
    const std::shared_ptr<const Material>& material =
        mesh->mMaterialIndex < materials.size() ? materials[mesh->mMaterialIndex] : kFallback;
    const auto base_index = static_cast<std::uint32_t>(out.vertices.size());

    out.vertices.reserve(out.vertices.size() + mesh->mNumVertices);
    for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
      Vertex vert;
      const aiVector3D& p = mesh->mVertices[v];
      glm::vec3 pos = glm::vec3(node_transform * glm::vec4(p.x, p.y, p.z, 1.0f));
      glm::vec3 nrm(0.0f, 0.0f, 1.0f);
      if (mesh->HasNormals()) {
        const aiVector3D& n = mesh->mNormals[v];
        nrm = glm::normalize(normal_matrix * glm::vec3(n.x, n.y, n.z));
      }
      vert.position = extra_rotation * pos;
      vert.normal = glm::normalize(extra_rotation * nrm);
      // Vertex color carries ONLY genuine per-vertex colors (glTF COLOR_0); the
      // material base color lives in SubMesh::material and is applied by the
      // shader. A white default leaves untextured/non-vertex-colored meshes to be
      // driven purely by the material factor (no double-application).
      vert.color =
          mesh->HasVertexColors(0)
              ? glm::vec4(mesh->mColors[0][v].r, mesh->mColors[0][v].g, mesh->mColors[0][v].b, mesh->mColors[0][v].a)
              : glm::vec4(1.0f);
      if (mesh->HasTextureCoords(0)) {
        const aiVector3D& uv = mesh->mTextureCoords[0][v];
        vert.uv = glm::vec2(uv.x, uv.y);
      }
      // Tangent basis for normal mapping (aiProcess_CalcTangentSpace). Transform
      // into world like the geometry, re-orthogonalize against the final normal
      // (Gram-Schmidt), and store the bitangent handedness in .w.
      if (mesh->HasTangentsAndBitangents()) {
        const aiVector3D& t = mesh->mTangents[v];
        const aiVector3D& b = mesh->mBitangents[v];
        glm::vec3 tan = extra_rotation * (linear * glm::vec3(t.x, t.y, t.z));
        glm::vec3 bit = extra_rotation * (linear * glm::vec3(b.x, b.y, b.z));
        tan = tan - vert.normal * glm::dot(vert.normal, tan);
        if (glm::dot(tan, tan) > 1e-12f) {
          tan = glm::normalize(tan);
          const float sign = glm::dot(glm::cross(vert.normal, tan), bit) < 0.0f ? -1.0f : 1.0f;
          vert.tangent = glm::vec4(tan, sign);
        }
      }
      out.vertices.push_back(vert);
    }
    const std::size_t range_start = out.indices.size();
    for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
      const aiFace& face = mesh->mFaces[f];
      // aiProcess_Triangulate guarantees 3 indices per face; skip any degenerate
      // line/point primitive assimp may still emit for stray geometry.
      if (face.mNumIndices != 3) {
        continue;
      }
      out.indices.push_back(base_index + face.mIndices[0]);
      out.indices.push_back(base_index + face.mIndices[1]);
      out.indices.push_back(base_index + face.mIndices[2]);
    }
    const std::size_t range_count = out.indices.size() - range_start;
    if (range_count > 0U) {
      out.submeshes.push_back(SubMesh{range_start, range_count, material});
    }
  }

  for (unsigned int c = 0; c < node->mNumChildren; ++c) {
    appendNode(scene, node->mChildren[c], node_transform, extra_rotation, materials, out);
  }
}

MeshData buildMeshData(const aiScene* scene, bool flip_to_z_up, const QString& source, const QString& base_dir) {
  MeshData out;
  if (scene == nullptr || scene->mRootNode == nullptr) {
    out.error = QStringLiteral("assimp returned no scene for %1").arg(source);
    return out;
  }
  // Resolve each material once (embedded-texture extraction is per-material, not
  // per-mesh) and index into it by aiMesh::mMaterialIndex while walking the graph.
  std::vector<std::shared_ptr<const Material>> materials;
  materials.reserve(scene->mNumMaterials);
  for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
    materials.push_back(std::make_shared<const Material>(readMaterial(scene, i, base_dir)));
  }
  const glm::mat3 extra = flip_to_z_up ? zUpFromYUpRotation() : glm::mat3(1.0f);
  appendNode(scene, scene->mRootNode, glm::mat4(1.0f), extra, materials, out);
  if (out.vertices.empty() || out.indices.empty()) {
    out.error = QStringLiteral("no renderable geometry in %1").arg(source);
    out.ok = false;
    return out;
  }
  out.ok = true;
  return out;
}

}  // namespace

bool MeshLoader::wantsZUpFlip(const QString& format_hint) {
  const QString h = format_hint.toLower();
  // glTF is Y-up by spec; assimp also normalizes Collada's <up_axis> to its
  // internal Y-up (verified empirically — see zUpFromYUpRotation). Both need the
  // explicit Y->Z flip into our Z-up world. STL/OBJ carry no up-axis convention
  // and assimp passes their coordinates through unrotated, so they stay false.
  return h == QLatin1String("glb") || h == QLatin1String("gltf") || h == QLatin1String("gltf2") ||
         h == QLatin1String("dae") || h == QLatin1String("collada");
}

MeshData MeshLoader::importFromFile(const QString& path, bool flip_to_z_up) {
  // Exception barrier: assimp drives allocations from file-declared counts and
  // buildMeshData reserves from them too, so hostile input can throw
  // bad_alloc/length_error. This runs on a QtConcurrent worker — an escaped
  // exception would be rethrown by QFuture::result() on the GUI thread (inside
  // paintGL) and terminate the app. Mirrors pointcloud_codecs.cpp's barriers.
  try {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path.toStdString(), kPostProcessFlags);
    if (scene == nullptr) {
      MeshData out;
      out.error = QString::fromUtf8(importer.GetErrorString());
      if (out.error.isEmpty()) {
        out.error = QStringLiteral("assimp failed to import %1").arg(path);
      }
      return out;
    }
    return buildMeshData(scene, flip_to_z_up, path, QFileInfo(path).absolutePath());
  } catch (const std::exception& ex) {
    MeshData out;
    out.error = QStringLiteral("mesh import of %1 threw: %2").arg(path, QString::fromUtf8(ex.what()));
    return out;
  } catch (...) {
    MeshData out;
    out.error = QStringLiteral("mesh import of %1 threw an unknown exception").arg(path);
    return out;
  }
}

MeshData MeshLoader::importFromMemory(const QByteArray& bytes, const QString& format_hint, bool flip_to_z_up) {
  // Same exception barrier as importFromFile (see the comment there).
  try {
    Assimp::Importer importer;
    const std::string hint = format_hint.toStdString();
    const aiScene* scene = importer.ReadFileFromMemory(
        bytes.constData(), static_cast<std::size_t>(bytes.size()), kPostProcessFlags,
        hint.empty() ? nullptr : hint.c_str());
    if (scene == nullptr) {
      MeshData out;
      out.error = QString::fromUtf8(importer.GetErrorString());
      if (out.error.isEmpty()) {
        out.error = QStringLiteral("assimp failed to import %1 buffer").arg(format_hint);
      }
      return out;
    }
    // Embedded buffers (glTF/GLB, the Waymo path) keep their per-submesh materials:
    // embedded textures are extracted into Material::base_color etc. by readMaterial,
    // so there is no external base_dir to resolve against and nothing to flatten.
    return buildMeshData(scene, flip_to_z_up, QStringLiteral("<memory:%1>").arg(format_hint), QString{});
  } catch (const std::exception& ex) {
    MeshData out;
    out.error = QStringLiteral("mesh import of %1 buffer threw: %2").arg(format_hint, QString::fromUtf8(ex.what()));
    return out;
  } catch (...) {
    MeshData out;
    out.error = QStringLiteral("mesh import of %1 buffer threw an unknown exception").arg(format_hint);
    return out;
  }
}

namespace {
// Compose the cache key from the resolved path and the effective flip. A NUL
// byte separates them: NUL cannot appear in a filesystem path, so the two flip
// variants of one path can never collide with each other or with another path.
QString meshCacheKey(const QString& resolved_path, bool flip) {
  return resolved_path + QLatin1Char('\0') + QLatin1Char(flip ? '1' : '0');
}
}  // namespace

QFuture<MeshData> MeshLoader::load(const QString& resolved_path, std::optional<bool> flip_override) {
  // assimp infers the format from the extension. The effective flip is the
  // caller's override when given, else the per-format default (wantsZUpFlip):
  // DAE/glTF arrive Y-up and flip; STL/OBJ stay as-is.
  const bool flip = flip_override.value_or(wantsZUpFlip(QFileInfo(resolved_path).suffix()));
  const QString key = meshCacheKey(resolved_path, flip);
  QMutexLocker lock(&cache_mutex_);
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    return it.value();
  }
  QFuture<MeshData> future =
      QtConcurrent::run([resolved_path, flip]() { return MeshLoader::importFromFile(resolved_path, flip); });
  cache_.insert(key, future);
  return future;
}

QFuture<MeshData> MeshLoader::loadFromMemory(const QByteArray& bytes, const QString& format_hint) {
  // Embedded buffers are not path-keyed (no stable identity); skip the cache.
  const bool flip = wantsZUpFlip(format_hint);
  return QtConcurrent::run(
      [bytes, format_hint, flip]() { return MeshLoader::importFromMemory(bytes, format_hint, flip); });
}

void MeshLoader::clearCache() {
  QMutexLocker lock(&cache_mutex_);
  cache_.clear();
}

void MeshLoader::evict(const QString& resolved_path) {
  QMutexLocker lock(&cache_mutex_);
  // The cache is keyed by path+effective-flip; remove both flip variants so a
  // Retry re-imports regardless of which override was in effect at load time.
  cache_.remove(meshCacheKey(resolved_path, false));
  cache_.remove(meshCacheKey(resolved_path, true));
}

}  // namespace pj::scene3d
