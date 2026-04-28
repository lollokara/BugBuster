#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void hub_recovery_init(void);   // call once after USB + PCA9535 init
void hub_recovery_tick(void);   // call from mainLoopTask each iteration
#ifdef __cplusplus
}
#endif
