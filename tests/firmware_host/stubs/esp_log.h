// Host stub: logging compiled out. A test may pre-define ESP_LOGW (e.g. via
// -include) to count warnings.
#pragma once
#define ESP_LOGE(tag, ...) ((void)(tag))
#ifndef ESP_LOGW
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGV(tag, ...) ((void)(tag))
