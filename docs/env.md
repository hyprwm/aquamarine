## Environment variables

Unless specified otherwise, a variable is enabled if and only if it's set to `1`

### DRM

`AQ_DRM_DEVICES` -> Set an explicit list of DRM devices (GPUs) to use. It's a colon-separated list of paths, with the first being the primary. E.g. `/dev/dri/card1:/dev/dri/card0`
`AQ_NO_ATOMIC` -> Disables drm atomic modesetting
`AQ_MGPU_NO_EXPLICIT` -> Disables explicit syncing on mgpu buffers
`AQ_NO_MODIFIERS` -> Disables modifiers for DRM buffers
`AQ_FAST_MODESET` -> Skips modeset if mode is unchanged

### Debugging

`AQ_TRACE` -> Enables trace (very verbose) logging
