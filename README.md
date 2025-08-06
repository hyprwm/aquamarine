## Aquamarine

Aquamarine is a very light linux rendering backend library. It provides basic abstractions
for an application to render on a Wayland session (in a window) or a native DRM session.

It is agnostic of the rendering API (Vulkan/OpenGL) and designed to be lightweight, performant, and
minimal.

Aquamarine provides no bindings for other languages. It is C++-only.

## Stability

Aquamarine depends on the ABI stability of the stdlib implementation of your compiler. Sover bumps will be done only for aquamarine ABI breaks, not stdlib.

## Building

```sh
cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr -S . -B ./build
cmake --build ./build --config Release --target all -j`nproc 2>/dev/null || getconf _NPROCESSORS_CONF`
```

Install with:

```sh
cmake --install ./build
```

## TODOs

 - [x] Wayland backend
 - [x] DRM backend (DRM / KMS / libinput)
 - [x] Virtual backend (aka. Headless)
 - [ ] Hardware plane support


