# glidepal — palettized textures and the chroma key over Glide

A test case that draws the same 64×64 `GR_TEXFMT_P_8` texture four times, each with a
different combination of chroma key and alpha blending, on top of an orange plate.
Wherever the transparency works the orange shows through; where it does not, a black
box stands there. One screenshot says which of the four paths is broken.

| Panel | chroma key | blending | alpha |
|---|---|---|---|
| 1 | on | off | iterated — the Diablo II case |
| 2 | **off** | off | iterated — the control, has to be a black box |
| 3 | on | on (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`) | iterated |
| 4 | on | on | from the texture |

## Building

    make host     # Linux, against OpenGLide      -> build/glidepal
    make win32    # cross for Windows 9x, MinGW   -> build/GLIDEPAL.EXE
    make run      # builds the host version and starts it

The same source runs natively on the host against OpenGLide and inside the guest
against the qemu-3dfx wrapper. That is the point: if the host picture is right and the
guest picture is wrong, the fault is in the passthrough; if both are wrong, it is in
OpenGLide.

The Glide declarations come from `../glidecube/src/glidemin.h` — one header for both
test cases, so a constant is only ever written down once. Override `OPENGLIDE` and
`WRAPPER_DLL` on the command line if the checkout layout differs.

## What it found

Two bugs in OpenGLide, both in the chroma key, both fixed:

1. `ConvertColor4B` rotated `GR_COLORFORMAT_RGBA` the wrong way, so Diablo II's black
   chroma key `0x000000FF` became a green `0x00FF00` that is in no palette.
2. The chroma key was only emulated while blending was off. Diablo II draws its menu
   text with `GR_BLEND_ZERO`/`GR_BLEND_SRC_COLOR`, a multiply, in which a keyed black
   texel blackens the pixel underneath.

Notably, this test case did **not** show the first bug: it opens with
`GR_COLORFORMAT_ARGB`, for which the conversion is the identity. What it did show was
that the passthrough is faultless — which is what narrowed the search down to
OpenGLide. See `docs/LOG.md` [236]–[239].
