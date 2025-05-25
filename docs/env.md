## Environment variables

Unless specified otherwise, a variable is enabled if and only if it's set to `1`

### DRM

### DRM Devices and Settings

`AQ_DRM_DEVICES` -> This option allows you to set a specific list of DRM devices (GPUs) for the system to use.  
  The list is separated by colons (:), and the first device listed will be the primary GPU.  
  For example: `/dev/dri/card1:/dev/dri/card0`.  
  This means the system will primarily use `card1` and then fall back to `card0` if needed.

`AQ_NO_ATOMIC` -> Disables atomic mode setting for the DRM.  
  Atomic mode setting is a feature that makes screen updates more reliable by grouping changes together.  
  Setting AQ_NO_ATOMIC to 1 can be useful on hardware or drivers that don’t fully support atomic mode setting,  
  as it can prevent crashes or freezes caused by incomplete or incorrect atomic updates.

`AQ_MGPU_NO_EXPLICIT` -> Disables explicit synchronization between buffers when using multiple GPUs (multi-GPU setups).  
  Without explicit syncing, the system handles buffer transfers between GPUs more automatically,  
  which can improve performance in some cases but might introduce errors in more complex setups.

`AQ_NO_MODIFIERS` -> Disables DRM modifiers for buffers.  
  This can also resolve issues where a monitor fails to turn on or is not detected properly.  
  DRM modifiers define how pixel data is stored in memory (e.g., in optimized formats).  
  Setting `AQ_NO_MODIFIERS` to `1` ensures simpler buffer handling,  
  which might be necessary on hardware that doesn’t support advanced memory layouts,  
  especially for high resolutions like 4K.

### Debugging

`AQ_TRACE` -> Enables trace (very verbose) logging
