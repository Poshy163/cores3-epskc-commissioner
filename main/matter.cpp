#include "matter.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_matter.h"
#include "esp_openthread_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "settings.h"

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/KvsPersistentStorageDelegate.h>
#include <platform/OpenThread/GenericThreadBorderRouterDelegate.h>
#include <setup_payload/OnboardingCodesUtil.h>

#if !defined(CONFIG_ESP_MATTER_ENABLE_OPENTHREAD)
#error "The CoreS3 Matter TBR requires CONFIG_ESP_MATTER_ENABLE_OPENTHREAD=y"
#endif

#if !defined(CONFIG_OPENTHREAD_BORDER_ROUTER)
#error "The CoreS3 Matter TBR requires CONFIG_OPENTHREAD_BORDER_ROUTER=y"
#endif

namespace {

constexpr char kTag[] = "matter";
constexpr char kMatterNvsPartition[] = "matter_nvs";
constexpr char kOpenThreadNvsPartition[] = "nvs";
constexpr gpio_num_t kRcpResetPin = GPIO_NUM_7;
constexpr gpio_num_t kRcpRxPin = GPIO_NUM_10;
constexpr gpio_num_t kRcpTxPin = GPIO_NUM_17;
constexpr uart_port_t kRcpUart = UART_NUM_1;
constexpr int kRcpBaudRate = 460800;

constexpr uint16_t kMinCommissioningWindowSeconds = 3 * 60;
constexpr uint16_t kMaxCommissioningWindowSeconds = 15 * 60;

std::atomic<matter_runtime_status_t> s_status{MATTER_RUNTIME_STOPPED};
std::atomic<esp_err_t> s_last_error{ESP_OK};
std::atomic<bool> s_started{false};
std::atomic<bool> s_commissioned{false};
std::atomic<bool> s_commissioning_window_open{false};
std::atomic<bool> s_window_request_pending{false};
std::atomic<bool> s_factory_reset_pending{false};
std::atomic<uint16_t> s_tbr_endpoint_id{0};

chip::KvsPersistentStorageDelegate s_tbr_storage_delegate;
chip::app::Clusters::ThreadBorderRouterManagement::GenericOpenThreadBorderRouterDelegate
    s_tbr_delegate(&s_tbr_storage_delegate);

esp_err_t chip_error_to_esp(CHIP_ERROR error)
{
    if (error == CHIP_NO_ERROR) {
        return ESP_OK;
    }
    if (error == CHIP_ERROR_INVALID_ARGUMENT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (error == CHIP_ERROR_BUFFER_TOO_SMALL) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (error == CHIP_ERROR_INCORRECT_STATE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

esp_err_t fail_init(esp_err_t error, const char *message)
{
    s_last_error.store(error, std::memory_order_relaxed);
    s_status.store(MATTER_RUNTIME_ERROR, std::memory_order_release);
    ESP_LOGE(kTag, "%s: %s", message, esp_err_to_name(error));
    return error;
}

bool partition_label_is(const char *configured_label, const char *expected_label)
{
    return configured_label != nullptr && std::strcmp(configured_label, expected_label) == 0;
}

esp_err_t validate_storage_configuration()
{
    /* Fail closed rather than silently placing fabric credentials in the
     * application's legacy NVS partition. */
    if (!partition_label_is(CONFIG_ESP_MATTER_NVS_PART_NAME, kMatterNvsPartition) ||
        !partition_label_is(CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL, kMatterNvsPartition) ||
        !partition_label_is(CONFIG_CHIP_CONFIG_NAMESPACE_PARTITION_LABEL, kMatterNvsPartition) ||
        !partition_label_is(CONFIG_CHIP_COUNTERS_NAMESPACE_PARTITION_LABEL, kMatterNvsPartition) ||
        !partition_label_is(CONFIG_CHIP_KVS_NAMESPACE_PARTITION_LABEL, kMatterNvsPartition)) {
        ESP_LOGE(kTag, "all Matter/CHIP NVS labels must be '%s'", kMatterNvsPartition);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t init_matter_nvs()
{
    esp_err_t error = nvs_flash_init_partition(kMatterNvsPartition);
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* This erases Matter state only. The legacy NVS partition, including
         * Wi-Fi credentials and the Thread dataset, remains untouched. */
        ESP_LOGW(kTag, "repairing the dedicated Matter NVS partition");
        error = nvs_flash_erase_partition(kMatterNvsPartition);
        if (error == ESP_OK) {
            error = nvs_flash_init_partition(kMatterNvsPartition);
        }
    }
    return error;
}

esp_err_t reset_rcp()
{
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<unsigned>(kRcpResetPin);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) {
        return error;
    }
    error = gpio_set_level(kRcpResetPin, 0);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    error = gpio_set_level(kRcpResetPin, 1);
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

esp_openthread_platform_config_t make_openthread_platform_config()
{
    esp_openthread_platform_config_t config{};
    config.radio_config.radio_mode = RADIO_MODE_UART_RCP;
    config.radio_config.radio_uart_config.port = kRcpUart;
    config.radio_config.radio_uart_config.uart_config.baud_rate = kRcpBaudRate;
    config.radio_config.radio_uart_config.uart_config.data_bits = UART_DATA_8_BITS;
    config.radio_config.radio_uart_config.uart_config.parity = UART_PARITY_DISABLE;
    config.radio_config.radio_uart_config.uart_config.stop_bits = UART_STOP_BITS_1;
    config.radio_config.radio_uart_config.uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.radio_config.radio_uart_config.uart_config.rx_flow_ctrl_thresh = 0;
    config.radio_config.radio_uart_config.uart_config.source_clk = UART_SCLK_DEFAULT;
    config.radio_config.radio_uart_config.rx_pin = kRcpRxPin;
    config.radio_config.radio_uart_config.tx_pin = kRcpTxPin;

    config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;

    /* Keep the already deployed OpenThread dataset in the legacy NVS
     * partition. Matter fabrics and attributes use matter_nvs instead. */
    config.port_config.storage_partition_name = kOpenThreadNvsPartition;
    config.port_config.netif_queue_size = 10;
    config.port_config.task_queue_size = 10;
    return config;
}

void refresh_fabric_state()
{
    const bool commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() != 0;
    s_commissioned.store(commissioned, std::memory_order_release);
    s_status.store(commissioned ? MATTER_RUNTIME_COMMISSIONED : MATTER_RUNTIME_COMMISSIONABLE,
                   std::memory_order_release);
}

void matter_event_callback(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    using chip::DeviceLayer::DeviceEventType::kCommissioningComplete;
    using chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed;
    using chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened;
    using chip::DeviceLayer::DeviceEventType::kFabricCommitted;
    using chip::DeviceLayer::DeviceEventType::kFabricRemoved;

    switch (event->Type) {
    case kCommissioningWindowOpened:
        s_commissioning_window_open.store(true, std::memory_order_release);
        break;
    case kCommissioningWindowClosed:
        s_commissioning_window_open.store(false, std::memory_order_release);
        break;
    case kCommissioningComplete:
    case kFabricCommitted:
    case kFabricRemoved:
        refresh_fabric_state();
        break;
    default:
        break;
    }
}

void open_commissioning_window_work(intptr_t argument)
{
    const auto timeout = chip::System::Clock::Seconds32(static_cast<uint32_t>(argument));
    auto &manager = chip::Server::GetInstance().GetCommissioningWindowManager();

    CHIP_ERROR error = CHIP_NO_ERROR;
    if (!manager.IsCommissioningWindowOpen()) {
        const auto advertisement =
            chip::Server::GetInstance().GetFabricTable().FabricCount() == 0
            ? chip::CommissioningWindowAdvertisement::kAllSupported
            : chip::CommissioningWindowAdvertisement::kDnssdOnly;
        error = manager.OpenBasicCommissioningWindow(timeout, advertisement);
    }
    s_window_request_pending.store(false, std::memory_order_release);
    if (error != CHIP_NO_ERROR) {
        s_last_error.store(chip_error_to_esp(error), std::memory_order_relaxed);
        ESP_LOGE(kTag, "could not open commissioning window: %" CHIP_ERROR_FORMAT, error.Format());
    }
}

} // namespace

extern "C" esp_err_t matter_init(void)
{
    matter_runtime_status_t expected = MATTER_RUNTIME_STOPPED;
    if (!s_status.compare_exchange_strong(expected, MATTER_RUNTIME_STARTING,
                                          std::memory_order_acq_rel)) {
        return s_started.load(std::memory_order_acquire) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = validate_storage_configuration();
    if (error != ESP_OK) {
        return fail_init(error, "invalid Matter storage configuration");
    }
    error = init_matter_nvs();
    if (error != ESP_OK) {
        return fail_init(error, "could not initialize matter_nvs");
    }

    char border_router_name[SETTINGS_DEVICE_NAME_BUFFER_SIZE];
    error = settings_get_device_name(border_router_name, sizeof(border_router_name));
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "using default Matter TBR name: %s", esp_err_to_name(error));
    }

    esp_matter::node::config_t node_config{};
    const size_t node_label_length = std::min(
        std::strlen(border_router_name),
        sizeof(node_config.root_node.basic_information.node_label) - 1);
    std::memcpy(node_config.root_node.basic_information.node_label,
                border_router_name, node_label_length);
    node_config.root_node.basic_information.node_label[node_label_length] = '\0';
    esp_matter::node_t *node = esp_matter::node::create(&node_config, nullptr, nullptr);
    if (node == nullptr) {
        return fail_init(ESP_ERR_NO_MEM, "could not create Matter root node");
    }

    CHIP_ERROR chip_error = s_tbr_storage_delegate.Init(
        &chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr());
    if (chip_error != CHIP_NO_ERROR) {
        return fail_init(chip_error_to_esp(chip_error), "could not initialize TBR storage delegate");
    }
    s_tbr_delegate.SetThreadBorderRouterName(
        chip::CharSpan(border_router_name, std::strlen(border_router_name)));

    esp_matter::endpoint::thread_border_router::config_t tbr_config{};
    tbr_config.thread_border_router_management.delegate = &s_tbr_delegate;
    esp_matter::endpoint_t *endpoint = esp_matter::endpoint::thread_border_router::create(
        node, &tbr_config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
    if (endpoint == nullptr) {
        return fail_init(ESP_ERR_NO_MEM, "could not create Matter TBR endpoint");
    }
    s_tbr_endpoint_id.store(esp_matter::endpoint::get_id(endpoint), std::memory_order_release);

    esp_matter::cluster_t *management_cluster = esp_matter::cluster::get(
        endpoint, chip::app::Clusters::ThreadBorderRouterManagement::Id);
    if (management_cluster == nullptr) {
        return fail_init(ESP_FAIL, "Matter TBR management cluster is missing");
    }
    error = esp_matter::cluster::thread_border_router_management::feature::pan_change::add(
        management_cluster);
    if (error != ESP_OK) {
        return fail_init(error, "could not enable Matter TBR PAN Change feature");
    }

    esp_openthread_platform_config_t openthread_config = make_openthread_platform_config();
    error = set_openthread_platform_config(&openthread_config);
    if (error != ESP_OK) {
        return fail_init(error, "could not configure Matter-owned OpenThread");
    }
    error = reset_rcp();
    if (error != ESP_OK) {
        return fail_init(error, "could not reset the ESP32-H2 RCP");
    }

    error = esp_matter::start(matter_event_callback);
    if (error != ESP_OK) {
        return fail_init(error, "ESP-Matter start failed");
    }

    chip_error = chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(
        chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_ApplicationControlled);
    if (chip_error != CHIP_NO_ERROR) {
        return fail_init(chip_error_to_esp(chip_error), "could not retain application-controlled Wi-Fi");
    }

    s_started.store(true, std::memory_order_release);
    s_commissioning_window_open.store(
        chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen(),
        std::memory_order_release);
    refresh_fabric_state();
    s_last_error.store(ESP_OK, std::memory_order_relaxed);

    ESP_LOGI(kTag, "Matter TBR endpoint %u started (%s)",
             static_cast<unsigned>(s_tbr_endpoint_id.load(std::memory_order_relaxed)),
             s_commissioned.load(std::memory_order_relaxed) ? "commissioned" : "commissionable");
    return ESP_OK;
}

extern "C" matter_runtime_status_t matter_get_status(void)
{
    return s_status.load(std::memory_order_acquire);
}

extern "C" const char *matter_status_string(matter_runtime_status_t status)
{
    switch (status) {
    case MATTER_RUNTIME_STOPPED:
        return "stopped";
    case MATTER_RUNTIME_STARTING:
        return "starting";
    case MATTER_RUNTIME_COMMISSIONABLE:
        return "commissionable";
    case MATTER_RUNTIME_COMMISSIONED:
        return "commissioned";
    case MATTER_RUNTIME_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

extern "C" esp_err_t matter_get_last_error(void)
{
    return s_last_error.load(std::memory_order_relaxed);
}

extern "C" bool matter_is_commissioned(void)
{
    return s_commissioned.load(std::memory_order_acquire);
}

extern "C" bool matter_is_commissioning_window_open(void)
{
    return s_commissioning_window_open.load(std::memory_order_acquire);
}

extern "C" uint16_t matter_get_thread_border_router_endpoint_id(void)
{
    return s_tbr_endpoint_id.load(std::memory_order_acquire);
}

extern "C" esp_err_t matter_open_commissioning_window(uint16_t timeout_seconds)
{
    if (!s_started.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_seconds < kMinCommissioningWindowSeconds ||
        timeout_seconds > kMaxCommissioningWindowSeconds) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_commissioning_window_open.load(std::memory_order_acquire)) {
        return ESP_OK;
    }

    bool expected = false;
    if (!s_window_request_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }
    CHIP_ERROR error = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        open_commissioning_window_work, static_cast<intptr_t>(timeout_seconds));
    if (error != CHIP_NO_ERROR) {
        s_window_request_pending.store(false, std::memory_order_release);
        return chip_error_to_esp(error);
    }
    return ESP_OK;
}

extern "C" esp_err_t matter_factory_reset(void)
{
    if (!s_started.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    bool expected = false;
    if (!s_factory_reset_pending.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* This is the complete ESP-Matter reset path: it erases the data-model
     * namespace, schedules deletion of every fabric, clears CHIP KVS/config,
     * and finally calls esp_restart(). */
    const esp_err_t cleanup_error = esp_matter::factory_reset();
    if (cleanup_error != ESP_OK) {
        /* The upstream API schedules the full CHIP reset and reboot even when
         * its preliminary ESP-Matter namespace erase reports an error. */
        s_last_error.store(cleanup_error, std::memory_order_relaxed);
        ESP_LOGW(kTag, "Matter namespace cleanup reported %s; full reset is scheduled",
                 esp_err_to_name(cleanup_error));
    }
    return ESP_OK;
}

extern "C" esp_err_t matter_get_onboarding_codes(char *qr_code, size_t qr_code_size,
                                                   char *manual_code, size_t manual_code_size)
{
    if (qr_code == nullptr || manual_code == nullptr || qr_code_size == 0 || manual_code_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    qr_code[0] = '\0';
    manual_code[0] = '\0';

    if (!s_started.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_commissioned.load(std::memory_order_acquire) &&
        !s_commissioning_window_open.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    chip::RendezvousInformationFlags rendezvous(chip::RendezvousInformationFlag::kOnNetwork);
#if defined(CONFIG_ENABLE_CHIPOBLE)
    if (!s_commissioned.load(std::memory_order_acquire)) {
        rendezvous.Set(chip::RendezvousInformationFlag::kBLE);
    }
#endif

    chip::MutableCharSpan qr_span(qr_code, qr_code_size);
    CHIP_ERROR error = GetQRCode(qr_span, rendezvous);
    if (error == CHIP_NO_ERROR) {
        chip::MutableCharSpan manual_span(manual_code, manual_code_size);
        error = GetManualPairingCode(manual_span, rendezvous);
    }
    if (error != CHIP_NO_ERROR) {
        qr_code[0] = '\0';
        manual_code[0] = '\0';
        return chip_error_to_esp(error);
    }
    return ESP_OK;
}
