#pragma once

sprite_t *image_load(char *name, int screen_w, int screen_h, uint8_t *picture_data, int picture_data_length);

void image_view(char *name, int screen_w, int screen_h, int screen_scale);
