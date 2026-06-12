# pj_scene3D — Architecture (as built)

How the 3D scene renders and how robot models get on screen. This is the
distilled, as-built successor to the executed planning docs
(`PHOTOREALISM_AND_URDF_MESH_PLAN.md`, `URDF_MESH_ASSET_RESOLUTION_DESIGN.md`,
`CODEX_PROMPTS_URDF_MESH.md` — all removed 2026-06-10; see git history for the
full design rationale). The WHAT lives in [REQUIREMENTS.md](./REQUIREMENTS.md).

## Rendering pipeline

Per frame, `SceneViewWidget::paintGL`:

```
layers + passes → SceneHdrFbo (multisample RGBA16F + DEPTH32F)
                → resolve blit (single-sample color + depth)
                → SsaoPass (R16F AO, box-blurred)   [reads resolved depth]
                → EdlPass  (R16F shade factor)      [reads resolved depth]
                → composite/present (fullscreen triangle → backing FBO)
```

- **HDR chain (0A).** The render FBO is created at the backing FBO's *achieved*
  MSAA sample count (`context()->format().samples()` — never assume the 4 that
  was requested). MSAA→MSAA blits with mismatched formats are illegal, so the
  resolve target is always single-sample; the present pass draws into
  `defaultFramebufferObject()` (never FBO 0 — QOpenGLWidget renders off-screen).
  When the chain is unavailable the widget falls back to direct-to-backing
  rendering (one warning per context).
- **Linear light + tonemap (0B).** All color inputs are linearized (colormaps,
  occupancy LUT, vertex/base colors, theme colors; mesh diffuse textures upload
  as `GL_SRGB8_ALPHA8`). The composite applies exposure → tonemap
  (None/ACES/AgX; AgX matrices are **column-major** GLSL `mat3` ctors) →
  saturation → manual sRGB encode.
- **Annotation alpha-marker.** The scene FBO's alpha is a per-pixel
  tonemap-bypass marker, not coverage: annotation passes (TF triads via
  `ArrowGizmo`, HUD overlay) write alpha 0 so they present flat and vivid,
  while data draws restore the marker with coverage-union blending
  `(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` on the alpha channel (preserving
  destination alpha instead would ghost occluded annotations through opaque
  meshes). Gizmo opacity rides the gizmo color's alpha through
  `glBlendFuncSeparate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ZERO, ONE_MINUS_SRC_ALPHA)`.
- **Background bypass.** Far-plane pixels (depth == 1) keep the exact theme
  color, ungraded — the background never shifts with the tonemap.
- **SSAO** reconstructs view position via `u_inv_proj` (works under the
  ortho camera; the perspective near/far formula does not) with an in-shader
  4×4-tiled hash as the rotation noise. **EDL** is Potree-derived (8 circular
  neighbours, log-depth response). Both multiply into the composite and
  degrade to no-ops when unavailable (`u_has_ao` / `u_has_edl`).
- **Defaults** (look-dev, 2026-06-10): ACES, exposure 1.1, saturation 1.2,
  SSAO on (strength 1, radius 0.5 m), EDL on (strength 1, radius 0.6 px).
  Runtime knobs: `SceneViewWidget::compositeParams()`, `ssaoPass()`,
  `edlPass()`, and the process-wide `meshShadingParams()` (roughness 0.6,
  f0 0.06, ambient 1.0, direct 1.15 — a stopgap for a future per-scene
  lighting object). Shader provenance/licenses: [`../THIRDPARTY.md`](../THIRDPARTY.md).

**GL context lifecycle (don't regress).** `QOpenGLWidget` recreates its context
on every ADS reparent. Every pass, layer, the HDR chain, and the present
program implement `releaseGL()` (wired to the dying context's
`aboutToBeDestroyed`) and rebuild lazily — VAOs/FBOs/textures are per-context,
never shared. The app deliberately does NOT set `AA_ShareOpenGLContexts`.

## URDF / robot-model subsystem

- **Parser** (`widgets/src/urdf_parser.{h,cpp}`): `QDomDocument`-based;
  reads `<link>` visuals/collisions (origin, geometry, material). Geometry is
  the `GeomShape` variant — box/cylinder/sphere primitives **and** meshes.
  `<joint>` is deliberately ignored: **TF owns kinematics**; the robot model is
  a decoration on the TF tree. xacro is detected (element prefix or filename)
  and rejected with an actionable error, never a cryptic XML failure.
- **Sources.** A `RobotModelLayer` reads its URDF from a store topic
  (`kRobotDescription`, decoded via the topic's parser — payload bytes are
  CDR-framed, never cast to a string), a local file, or an http(s) URL.
  File/URL layers are dock-local: synthetic registry ids that never touch the
  ObjectStore (`Scene3DDockWidget::addRobotModelLayer{,FromUrl}`).
- **Mesh loading** is async (`QtConcurrent::run` → futures polled in
  `render()`); the GL thread never blocks. assimp covers STL/DAE/OBJ/glTF.
  `MeshData` carries UV0 + tangents (`aiProcess_CalcTangentSpace`) and a
  per-`SubMesh` `Material` (glTF 2.0 metallic-roughness, read via assimp's
  material abstraction). Each map (`TextureSource`) is an external file path OR
  inline bytes for embedded glTF/GLB `*N` images (still PNG/JPEG-encoded), keyed
  by content hash; `MeshRenderPass` decodes both, uploads color/emissive sRGB and
  data maps (metallic-roughness/normal/occlusion) linear, and caches per-context
  by key plus texture color space. Sources without PBR factors fall back to
  `MeshShadingParams`.
- **Rendering**: `MeshRenderPass` (metallic-roughness GGX + normal mapping +
  occlusion + emissive + hemispheric ambient + camera headlight; no IBL, so
  metals get specular highlights but no environment reflection) draws meshes by
  key and primitives from unit
  box/cylinder/sphere geometry (URDF cylinder is Z-aligned; sizes ride the
  model matrix). Unresolved meshes render a **magenta unit cube** —
  intentionally ugly, impossible to mistake for data. Visual meshes are split
  into opaque and best-effort translucent draws (layer opacity, override alpha,
  or glTF `BLEND`; no depth sort); collision meshes draw as a translucent
  overlay after opaque geometry. Scene-wide opacity/visibility comes from
  `meshShadingParams()`.
- **Time contract**: `RobotModelLayer::timeRange()` returns the inverted
  sentinel `{Timepoint::max(), Timepoint::min()}` — a static decoration must
  neither widen the playback timeline (`{0, INT64_MAX}` would balloon it to
  2262) nor clamp the playhead to its latch timestamp. The dock skips
  inverted ranges and still delivers live tracker time.

## Asset resolution (`package://` for a non-ROS app)

`UrdfPackageResolver` — URI scheme dispatch first:
`file://` → absolute path; bare path → relative to the URDF's directory
(never enters the package chain); `http(s)://` → allowed only for URL-source
layers; `package://pkg/rel` → the chain below, stop at first hit:

1. **Remembered per-MCAP mapping** — QSettings
   `pj_scene3d/urdf_per_mcap_packages`, keyed by canonical MCAP path.
2. **Ancestor heuristic** — walk up from the URDF's dir (≤10 levels) looking
   for a directory whose basename == `pkg`, accepting only if
   `candidate/rel` exists (prevents wrong-folder false positives). URL
   sources use the Foxglove URL-segment variant.
3. **Search roots** — ordered list auto-seeded from the URDF dir, the MCAP's
   dir, `$ROS_PACKAGE_PATH`, and `$AMENT_PREFIX_PATH`/`$COLCON_PREFIX_PATH`
   (+`/share`), all via `qEnvironmentVariable` — zero ROS dependency.
   Package identity is directory basename only; no `package.xml` check.
4. **Ask once** — unresolved packages surface on the layer's status text;
   a chosen root is stored per-MCAP and globally, then pending meshes retry.

Failure semantics: never silent. Missing meshes → magenta cubes + status
counts; wrong folder picked → explicit "expected a subdirectory named <pkg>"
error; URDF latch not yet received → 500 ms-throttled re-check with a 10 s
timeout message; HTTP mesh fetches are capped at 4 concurrent.

## Scene-controls panel (pj_app)

`Scene3DConfigPanel` (right side-panel) drives scene-wide state, persisted in
QSettings `pj_scene3d/scene_controls/*` and re-applied to every dock it binds:
grid style/size/divisions/visibility (`GridRenderPass` rebuilds geometry
lazily at render time), TF-frame size/opacity/visibility, mesh/collision
opacity/visibility, and the **Model/URDF** row — a File/Topic/URL source
combo plus an add button; each added robot gets a row with a remove button.
Robot layers are panel-managed: filtered out of the Topics list (the
per-layer config widget — frame prefix, color override, COLLADA up_axis,
resolution override — still exists on the layer but is not reachable from
the panel; resurrect behind an "advanced" disclosure if missed). The
phases-0B/D/B look knobs are runtime APIs with baked defaults — no app UI;
the `scene3d_mesh_viewer` demo exposes them for look-dev.
