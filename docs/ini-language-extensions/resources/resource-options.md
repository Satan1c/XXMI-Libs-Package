# Resource Options

## `override_color_space`

Controls the color space used when loading a texture resource.

By default, the texture's color space is determined by the texture format and/or metadata. 

`override_color_space` allows to explicitly override that behavior:
* `sRGB` — force the texture to be loaded as an sRGB texture.
* `Linear` — force the texture to be loaded as a linear texture.

```ini
[ResourceTexture]
filename = Texture.dds
override_color_space = sRGB
```

## `uav_counter`

Sets the hidden counter of an append or consume buffer whenever the resource is bound to a UAV slot.

```ini
[ResourceAppendBuffer]
uav_counter = 0
```

The value is passed as the slot's `pUAVInitialCounts` entry. Unset, it stays `-1`, which leaves the current count alone — the behaviour before this key existed. Single-slot binds and ranges read it the same way:

```ini
cs-u0 = ref ResourceAppendBuffer
cs-u[0:3] = ref PoolAppendBuffers
```

Being declared on the resource, the count applies to every bind, so it cannot be varied per draw call. A bind skipped by `unless_null` does not apply it.

D3D11 only honours the counter for views created with `D3D11_BUFFER_UAV_FLAG_APPEND` or `D3D11_BUFFER_UAV_FLAG_COUNTER`. Views created for a custom resource never carry either flag, so the key reaches only a view the game created and this resource is holding, such as one captured with `ResourceFoo = ref cs-u0` and bound again elsewhere.
