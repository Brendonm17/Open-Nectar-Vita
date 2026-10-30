#ifndef PC_PHOTO_MODE_H
#define PC_PHOTO_MODE_H

// Photo mode: frozen world, free camera.

#ifdef __cplusplus
extern "C" {
#endif

/// True while photo mode is on.
int pc_photo_mode_active(void);

/// Edge-detected toggle key. Call once per frame.
int pc_photo_mode_poll_toggle(void);

/// Seeds the free camera from the game camera pose.
void pc_photo_mode_enter(float posX, float posY, float posZ,
                         float pitch, float yaw);

void pc_photo_mode_exit(void);

/// Advances the free camera; returns the camera pose.
void pc_photo_mode_update(float dt,
                          float* posX, float* posY, float* posZ,
                          float* pitch, float* yaw, float* roll);

/// View direction for a pitch and yaw.
void pc_photo_mode_forward_vector(float pitch, float yaw,
                                  float* outX, float* outY, float* outZ);

/// Horizontal right vector.
void pc_photo_mode_right_vector(float yaw, float* outX, float* outY, float* outZ);

/// Pitch and yaw from a view direction.
void pc_photo_mode_angles_from_forward(float fx, float fy, float fz,
                                       float* outPitch, float* outYaw);

#ifdef __cplusplus
}
#endif

#endif // PC_PHOTO_MODE_H
