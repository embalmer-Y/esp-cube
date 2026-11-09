# cubecore: 公共基础设施(无业务逻辑)
idf_component_register(
    SRCS "cube_event_bus.c" "cube_ringbuf.c" "cube_resampler.c" "cube_fft.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_common
)
