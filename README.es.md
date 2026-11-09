# redroid-hwenc

**Idiomas:** [English](README.md) | Español | [中文](README.zh-CN.md)

> El idioma oficial de este proyecto es el **inglés**. Este archivo se ofrece por comodidad y
> puede no estar perfectamente actualizado o traducido — ante cualquier duda, [README.md](README.md)
> (inglés) es la fuente de verdad.

Encoder de video por hardware (H.264/H.265, vía VA-API) para [redroid](https://github.com/remote-android/redroid-doc).

> **¿Solo querés que esto funcione?** El código de este proyecto es lo que hace andar a los
> módulos `hwenc`/`hwdec` de [**redroid-forge**](https://github.com/fogelmanjg/redroid-forge), un
> gestor completo de contenedores Android construido sobre esta base (GApps, traducción ARM,
> WiFi, backups, una interfaz real). Si querés una instancia funcionando, empezá por ahí — este
> repo es donde vive y se documenta la ingeniería inversa de base, tier por tier, para quien quiera
> entenderla o construir sobre ella, no solo usar el resultado.

## El problema

BlueStacks, Nox y MuMu no tienen equivalente real en Linux desktop. [redroid](https://github.com/remote-android/redroid-doc)
es lo más cercano — Android corriendo en un contenedor Docker, con GPU passthrough para el
renderizado — pero el streaming de esa pantalla (scrcpy, o cualquier consumo de video dentro de
una app Android) sigue cayendo en un **encoder de software puro**
(`OMX.google.h264.encoder` / `c2.android.avc.encoder`). `gpuMode=host` acelera el renderizado
(OpenGL/Vulkan vía Mesa), nunca el encoder.

Este pedido lleva **abierto desde 2022** en el repo oficial, sin que nadie lo haya resuelto y
publicado:

- [remote-android/redroid-doc#126](https://github.com/remote-android/redroid-doc/issues/126) — AMD VA-API (OMX/Codec2)
- [remote-android/redroid-doc#172](https://github.com/remote-android/redroid-doc/issues/172) — mismo pedido, AMD
- [remote-android/redroid-doc#168](https://github.com/remote-android/redroid-doc/issues/168) — mismo pedido, Intel
- [remote-android/redroid-doc#535](https://github.com/remote-android/redroid-doc/issues/535) — pedido de H.265, comentario de septiembre 2025 sin respuesta

El maintainer del proyecto (`zhouziyang`) siempre contesta lo mismo: los drivers VA-API ya están
empaquetados en redroid, pero el componente Codec2/OMX que los use para encodear queda como
tarea para la comunidad. En 4 años, nadie lo terminó y publicó.

## Por qué ahora

No es que sea imposible — es que la intersección de "le importa este problema específico" y
"está dispuesto a meterse en AOSP/Codec2/VA-API" es rara. La mayoría de la gente en esos issues
son usuarios pidiendo el feature, no gente dispuesta a escribirlo. Este repo es el intento de
resolverlo en público, en tiers de dificultad creciente, documentando el proceso a medida que
avanza (ver [DEVLOG.md](DEVLOG.md), en inglés).

## Roadmap (por dificultad, no por tiempo)

Cada tier asume que el anterior está resuelto. El ⭐ marca el checkpoint de mayor apalancamiento.
Esta lista dice qué se esperaba y qué resultó — para los bugs reales, los callejones sin salida y
cómo se llegó a cada resultado, ver [DEVLOG.md](DEVLOG.md) (sesión por sesión, en inglés) y,
donde exista, el README de la propia subcarpeta del tier, linkeado abajo.

- [x] **Tier 0 — Investigación pura.** Confirmado que la selección de codec es por capacidad, no
      hardcodeada, y que existen entrypoints reales de encode VA-API en este hardware.
- [x] **Tier 1 — Lectura/mapeo.** Mapeada la estructura de un componente Codec2 y la superficie de
      API de encode VA-API.
- [x] **Tier 2 — Prototipo nativo aislado.** Programa standalone en el host encodea H.264 real vía
      VA-API. → [tier2-vaapi-encode/](tier2-vaapi-encode/)
- [x] **Tier 3 — ⭐ El spike que decide todo.** Confirmado que un buffer gralloc exporta un
      dma-buf que VA-API importa zero-copy dentro de un contenedor redroid real — en tres GPUs
      distintas — la pregunta exacta que 4 años de issues en upstream nunca contestaron.
      → [tier3-dmabuf-import/README.md](tier3-dmabuf-import/README.md)
- [x] **Tier 4 — Esqueleto Codec2 en Android.** Un componente que Android reconoce y lista vía
      `dumpsys media.c2`, todavía sin encoding real conectado.
      → [tier4-codec2-skeleton/README.md](tier4-codec2-skeleton/README.md)
- [x] **Tier 5 — ⭐ Integración real. El objetivo de fondo.** Una app real, sin modificar
      (`scrcpy`), graba H.264 correcto codificado por hardware a través del camino genuino del
      framework — confirmado en 3 GPUs entre AMD/Intel (ver Compatibilidad de hardware más abajo).
      Agregados posteriores: P-frames reales (5.14, rotos en Intel), decodificación por hardware
      vendor-agnóstica para apps (5.15), y un segundo backend de encode basado en libavcodec que
      arregla el bug de P-frames de Intel (5.16). → [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md)
- [x] **Tier 6 (condicional al Tier 0).** Resultó innecesario — el Tier 0 ya había confirmado que
      la selección de codec es por capacidad, así que `scrcpy` adopta el encoder nuevo solo.
- [x] **Tier 7 — ⭐ Decodificación de video por hardware, una cuarta GPU (NVIDIA).** Decode NVDEC
      bit-exacto vía VA-API, conectado de punta a punta al daemon y a un componente Codec2 real
      `c2.hardware.decoder.h264` (ver Compatibilidad de hardware más abajo). **Próximo paso**: la
      prueba `MediaCodec` en dispositivo real, el mismo hito que el Tier 5 necesitó para encode.
      → [tier6-vaapi-decode/README.md](tier6-vaapi-decode/README.md), [tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md)
- [x] **Tier 8 — Android 11 (HIDL Codec2).** El Codec2 AIDL (sobre el que se construyen los Tiers
      4–7) no existe antes de Android 14 — confirmado contra el código fuente de AOSP en
      android-11.0.0_r48, 12.0.0_r34 y 13.0.0_r83. Un port a HIDL del mismo componente, más un fix
      a nivel kernel de `libion.so` que esas versiones viejas de AOSP necesitan en cualquier
      kernel de host moderno, logra el mismo encode por hardware — **confirmado de punta a punta
      en Android 11, tanto en AMD (Ryzen 5700G/Renoir) como en Intel (Iris Xe)**. Intel necesitó
      el backend de encode `ffmpeg` (5.16) para arreglar un bug de bitstream malformado
      (`Overread SPS by N bits`) en el backend por defecto — confirmado el 09/11/2026 como la
      misma clase de problema específico de Intel que el bug de P-frames del 5.14, no uno
      separado como se sospechaba al principio. **Android 12/13 se asumen funcionando de la misma
      forma, sin probarse por separado** — mismo hueco a nivel de AOSP, mismo fix, sin lógica
      específica de versión en el propio componente, así que no hay motivo para esperar que se
      comporten distinto ahí. Simplemente todavía no se corrió en hardware real de 12/13. Si lo
      probás y encontrás algo que no anda, es exactamente lo que esa suposición podría estar
      pasando por alto — abrí un issue con lo que viste y se va a revisar puntualmente. El lado
      de decode HIDL (`VaapiDecComponent`) tampoco se ejercitó todavía, misma situación.
      → [tier8-android11-13-hidl/README.md](tier8-android11-13-hidl/README.md)

Aunque no se avance más, cada tier por sí solo ya es una contribución publicable — nada de esto
quedó documentado por nadie hasta ahora.

## Compatibilidad de hardware

Resultados reales en hardware real, no especulación. "Pipeline completo" significa que una app
real, sin modificar, graba video correcto a través del camino genuino del framework de Android —
ver los README de cada tier y el DEVLOG.md para el detalle de cuál fue el fix real de cada GPU.

| GPU | Arquitectura | Estado | Notas |
|---|---|---|---|
| AMD Renoir (APU Ryzen, integrada) | GFX9 | **Pipeline completo funciona** | Implementación de referencia. P-frames (5.14) andan; backend `ffmpeg` (5.16) también validado acá. |
| AMD Radeon RX 480 (Polaris, discreta) | GFX8 | **Pipeline completo funciona** | Sin modifier DRM disponible para esta GPU/formato — necesitó el camino viejo de importación pre-modifier de VA-API. P-frames (5.14) andan. **Evitar el backend `ffmpeg` (5.16) acá**: cuelgues reales de GPU durante la validación (timeout, reset completo, uno que necesitó reiniciar el host) — quedarse en el backend por defecto (`legacy`). |
| Intel Iris Xe (TigerLake-LP, integrada) | Gen12 | **Pipeline completo funciona** | Misma imagen de vendor sin modificar que AMD, sin rebuild. Un bug de tiling específico del vendor, arreglado en el daemon. P-frames en el backend por defecto (`legacy`) están **rotos** (5.14: ~60% de los cuadros fallan al decodificar) — usar el backend `ffmpeg` (5.16) acá para P-frames funcionando. |

Decodificación por hardware vendor-agnóstica **para apps** (5.15, `hwdec.c` — H.264/HEVC/VP9,
separado de la tabla de encode de arriba y del decode exclusivo de NVIDIA del Tier 7): confirmado
bit-exacto contra decode por software en las tres GPUs de arriba, ver
[tier5-vaapi-daemon/README.md](tier5-vaapi-daemon/README.md#515-vendor-agnostic-hardware-decode-hwdecc).

Esta lista tiene 3 máquinas porque eso es lo que este proyecto tiene a mano, no un reclamo de
completitud. Si tenés otro hardware y querés ayudar a completar esto, ver Contribuir más abajo —
un PR contra esta tabla con tus propios resultados es exactamente el tipo de contribución que
este proyecto puede aprovechar.

### Decode (Tier 7, NVIDIA)

Una tabla separada — decode es un pipeline distinto (el backend de decode de `tier5-vaapi-daemon`,
`nvidia-vaapi-driver`/NVDEC vía VA-API) de la tabla de encode de arriba, y hoy exclusivo de NVIDIA.

| GPU | Arquitectura | Estado | Notas |
|---|---|---|---|
| GTX 1050 Ti | Pascal | **Confirmado bit-exacto** | Verificado vía PSNR contra decode por software (`y:inf u:inf v:inf average:inf`), a través del daemon/stack Codec2 real, no solo el spike standalone. |
| RTX 4060 | Ada Lovelace | **Todavía no corrido acá** | El código de decode propio de este proyecto todavía no se ejercitó en esta GPU. Es razonable asumir que funciona (mismo mecanismo que la Pascal de arriba), pero según el propio estándar de este proyecto de "resultados reales, no especulación", tratarlo como sin verificar hasta que se corra de verdad y se chequee por PSNR. |

**Por qué NVIDIA no está en la tabla de encode de arriba**: `nvidia-vaapi-driver` es solo-decode
(`vainfo` lista cero entries `VAEntrypointEncSlice`) — un desajuste de mecanismo, no una brecha de
madurez. Aun así, NVIDIA **no** se queda sin aceleración ni sin encode en general: las dos cosas
viven en el proyecto hermano, [**redroid-nvidia**](https://github.com/fogelmanjg/redroid-nvidia),
vía un mecanismo distinto (un pipeline Venus-proxy propio + NVENC) — confirmado funcionando en las
dos GPUs de la tabla de arriba, con una limitación de driver abierta y específica de la GPU en la
RTX 4060 (ver ese repo para el detalle).

## Contribuir

Sin CLA, sin fricción — Apache-2.0 llano. Si te interesa este problema, un PR o un comentario en
un issue vale más que pedir permiso primero.

## Licencia

Apache License 2.0 — ver [LICENSE](LICENSE). Misma licencia que AOSP (`frameworks/av`), a
propósito: si algo de esto eventualmente vale la pena subirlo río arriba, no hay fricción legal.
