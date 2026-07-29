if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/common")
	set(common_dir "${CMAKE_CURRENT_SOURCE_DIR}/common")
else()
	set(common_dir "../../common")
endif()

# Upstream's standalone project restricts the whole build to an explicit
# top-level COMPONENTS list (see the original slave/CMakeLists.txt), which
# under older ESP-IDF's "full requires" behavior makes every component's
# public headers visible to every other component without this component
# declaring its own REQUIRES. Our project has no such restriction (plain
# project(ESP32C6), default strict per-component requires), so this needs
# its own explicit list -- inferred from the headers this component's
# sources actually #include (esp_console.h, nvs_flash.h, protocomm.h,
# esp_private/wifi.h, esp_bt.h, ...). UNVERIFIED beyond what's needed to get
# past compilation; may still be incomplete.
set(COMPONENT_REQUIRES
	nvs_flash
	esp_wifi
	esp_event
	esp_netif
	protocomm
	console
	driver
	esp_timer
	esp_pm
	wpa_supplicant
	esp_driver_sdio
	esp_app_format
	bootloader_support
	app_update
	esp_partition
	vfs
	esp_coex
)

# esp_bt only exists to require when the target actually supports BT and
# CONFIG_SOC_BT_SUPPORTED registers the component -- BT is out of scope here
# (CONFIG_ESP_HOSTED_CP_BT=n) and requiring it unconditionally fails CMake
# component resolution ("Failed to resolve component 'esp_bt'").
if(CONFIG_SOC_BT_SUPPORTED)
	list(APPEND COMPONENT_REQUIRES esp_bt)
endif()

set(COMPONENT_SRCS
	"${common_dir}/proto/esp_hosted_rpc.pb-c.c"
	"${common_dir}/utils/esp_hosted_cli.c"
	# Vendored protobuf-c runtime (common/protobuf-c/protobuf-c/), not fetched
	# via a registry component as upstream's own idf_component.yml would --
	# that manifest also pulled unrelated console/iperf/wifi-cmd example deps
	# we don't want, so it was dropped and this is compiled directly instead.
	"${common_dir}/protobuf-c/protobuf-c/protobuf-c.c"
	"protocomm_pserial.c"
	"esp_hosted_coprocessor.c"
	"stats.c"
	"host_power_save.c"
	"slave_light_sleep.c"
	"nw_split_router.c"
	"slave_transport_gpio_pin_guard.c"
	"slave_gpio_expander.c"
	"slave_ext_coex.c"
	"slave_control.c"
	"slave_util.c"
)

if(CONFIG_ESP_HOSTED_USE_MEMPOOL)
	list(APPEND COMPONENT_SRCS
		"${common_dir}/mempool/mempool_ll.c"
		"${common_dir}/mempool/mempool.c"
	)
endif()

if(CONFIG_SOC_BT_SUPPORTED)
	list(APPEND COMPONENT_SRCS "slave_bt.c")
endif()

if(CONFIG_ESP_HOSTED_CP_WIFI)
	list(APPEND COMPONENT_SRCS "slave_wifi_std.c")
endif()

if(CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT)
	list(APPEND COMPONENT_SRCS "slave_wifi_enterprise.c")
endif()

if (CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED)
	list(APPEND COMPONENT_SRCS "slave_network_split.c")
endif()

if (CONFIG_ESP_HOSTED_OT_RCP_ENABLED)
	list(APPEND COMPONENT_SRCS "slave_openthread.c")
endif()

set(COMPONENT_ADD_INCLUDEDIRS
	"."
	"${common_dir}"
	"${common_dir}/log"
	"${common_dir}/proto"
	"${common_dir}/rpc"
	"${common_dir}/transport"
	"${common_dir}/mempool/include"
	"${common_dir}/protobuf-c"
)

if(CONFIG_SOC_BT_SUPPORTED)
# Select BT UART code based on IDF Target
if(CONFIG_IDF_TARGET_ESP32)
	list(APPEND COMPONENT_SRCS slave_bt_uart_esp32.c)
elseif(CONFIG_IDF_TARGET_ESP32C3 OR CONFIG_IDF_TARGET_ESP32S3)
	list(APPEND COMPONENT_SRCS slave_bt_uart_esp32c3_s3.c)
else()
	list(APPEND COMPONENT_SRCS slave_bt_uart_esp32xx.c)
endif()
endif()

if(CONFIG_ESP_SDIO_HOST_INTERFACE)
	list(APPEND COMPONENT_SRCS sdio_slave_api.c)
elseif(CONFIG_ESP_SPI_HOST_INTERFACE)
	list(APPEND COMPONENT_SRCS spi_slave_api.c)
elseif(CONFIG_ESP_SPI_HD_MODE)
	list(APPEND COMPONENT_SRCS spi_hd_slave_api.c)
else(CONFIG_ESP_UART_HOST_INTERFACE)
	list(APPEND COMPONENT_SRCS uart_slave_api.c)
endif()

if(CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER)
	list(APPEND COMPONENT_SRCS example_peer_data_transfer.c)
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_LIGHT_SLEEP)
	list(APPEND COMPONENT_SRCS example_light_sleep.c)
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_MQTT)
	list(APPEND COMPONENT_SRCS example_mqtt_client.c)
endif()

if(CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT)
	list(APPEND COMPONENT_SRCS example_http_client.c)
endif()

# cli
list(APPEND COMPONENT_ADD_INCLUDEDIRS "${common_dir}/utils")
list(APPEND COMPONENT_SRCS "${common_dir}/utils/esp_hosted_cli.c")

register_component()

# Add directory of protocomm_priv.h to include paths
idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")

# Add linker options to wrap esp_wifi_init function
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_init")

if(DEFINED ENV{ESP_HOSTED_CI_PEDANTIC})
	target_compile_options(${COMPONENT_LIB} PRIVATE
		-Werror -Werror=deprecated-declarations -Werror=unused-variable
		-Werror=unused-but-set-variable -Werror=unused-function
		$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
endif()
