"""Exercise production Matter identity installation at Basic Information init.

The SDK stub deliberately replaces the global provider during startup, then
retains the provider reference during cluster initialization as ESP-Matter 1.6
does. No device or managed components are required.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
using CHIP_ERROR = int;
using esp_err_t = int;
constexpr int CHIP_NO_ERROR = 0;
constexpr int CHIP_ERROR_BUFFER_TOO_SMALL = 1;
constexpr int CHIP_ERROR_INCORRECT_STATE = 2;
constexpr int CHIP_ERROR_NOT_IMPLEMENTED = 3;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_INVALID_STATE = 4;
constexpr int ESP_FAIL = 5;
#define VerifyOrReturnError(condition, error) do { if (!(condition)) return error; } while (0)
namespace chip {
struct MutableByteSpan { uint8_t *data; size_t size; };
namespace app {
enum class ClusterShutdownType { kEndpointShutdown };
namespace Clusters::BasicInformation {
constexpr uint32_t Id = 0x0028;
enum class ProductFinishEnum { kMatte };
enum class ColorEnum { kBlue };
}
}
namespace DeviceLayer {
class DeviceInstanceInfoProvider {
public:
    virtual ~DeviceInstanceInfoProvider() = default;
    virtual CHIP_ERROR GetVendorName(char *, size_t) = 0;
    virtual CHIP_ERROR GetProductName(char *, size_t) = 0;
    virtual CHIP_ERROR GetHardwareVersionString(char *, size_t) = 0;
    virtual CHIP_ERROR GetVendorId(uint16_t &) = 0;
    virtual CHIP_ERROR GetProductId(uint16_t &) = 0;
    virtual CHIP_ERROR GetPartNumber(char *, size_t) = 0;
    virtual CHIP_ERROR GetProductURL(char *, size_t) = 0;
    virtual CHIP_ERROR GetProductLabel(char *, size_t) = 0;
    virtual CHIP_ERROR GetSerialNumber(char *, size_t) = 0;
    virtual CHIP_ERROR GetManufacturingDate(uint16_t &, uint8_t &, uint8_t &) = 0;
    virtual CHIP_ERROR GetHardwareVersion(uint16_t &) = 0;
    virtual CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan &) = 0;
    virtual CHIP_ERROR GetProductFinish(app::Clusters::BasicInformation::ProductFinishEnum *) = 0;
    virtual CHIP_ERROR GetProductPrimaryColor(app::Clusters::BasicInformation::ColorEnum *) = 0;
    virtual CHIP_ERROR GetJointFabricMode(uint8_t &) = 0;
};
static DeviceInstanceInfoProvider *provider;
DeviceInstanceInfoProvider *GetDeviceInstanceInfoProvider() { return provider; }
void SetDeviceInstanceInfoProvider(DeviceInstanceInfoProvider *next) { provider = next; }
}
}
namespace esp_matter {
namespace cluster {
using initialization_callback_t = void (*)(uint16_t);
using shutdown_callback_t = void (*)(uint16_t, chip::app::ClusterShutdownType);
}
struct cluster_t { cluster::initialization_callback_t init; cluster::shutdown_callback_t shutdown; };
struct endpoint_t { cluster_t *basic; };
struct node_t { endpoint_t *root; };
static int callback_error, callback_writes;
namespace endpoint {
endpoint_t *get(node_t *node, uint16_t endpoint) {
    assert(endpoint == 0); return node ? node->root : nullptr;
}
}
namespace cluster {
cluster_t *get(endpoint_t *root, uint32_t id) {
    assert(id == chip::app::Clusters::BasicInformation::Id); return root->basic;
}
initialization_callback_t get_init_callback(cluster_t *cluster) { return cluster->init; }
shutdown_callback_t get_shutdown_callback(cluster_t *cluster) { return cluster->shutdown; }
esp_err_t set_init_and_shutdown_callbacks(cluster_t *cluster, initialization_callback_t init,
                                         shutdown_callback_t shutdown) {
    ++callback_writes;
    if (callback_error) return callback_error;
    cluster->init = init; cluster->shutdown = shutdown; return ESP_OK;
}
}
}
'''

TEST = r'''
class SdkProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    int metadata_calls = 0, name_calls = 0;
    CHIP_ERROR metadata_error = CHIP_NO_ERROR;
    CHIP_ERROR text(char *buf, size_t size, const char *value) {
        ++metadata_calls;
        return metadata_error ? metadata_error : copy_string(buf, size, value);
    }
    CHIP_ERROR GetVendorName(char *buf, size_t size) override {
        ++name_calls; return copy_string(buf, size, "TEST_VENDOR");
    }
    CHIP_ERROR GetProductName(char *buf, size_t size) override {
        ++name_calls; return copy_string(buf, size, "TEST_PRODUCT");
    }
    CHIP_ERROR GetHardwareVersionString(char *buf, size_t size) override {
        ++name_calls; return copy_string(buf, size, "TEST_HARDWARE");
    }
    CHIP_ERROR GetVendorId(uint16_t &value) override {
        ++metadata_calls; value = 0xfff1; return metadata_error;
    }
    CHIP_ERROR GetProductId(uint16_t &value) override {
        ++metadata_calls; value = 0x8000; return metadata_error;
    }
    CHIP_ERROR GetPartNumber(char *buf, size_t size) override { return text(buf, size, "SDK_PART"); }
    CHIP_ERROR GetProductURL(char *buf, size_t size) override { return text(buf, size, "SDK_URL"); }
    CHIP_ERROR GetProductLabel(char *buf, size_t size) override { return text(buf, size, "SDK_LABEL"); }
    CHIP_ERROR GetSerialNumber(char *buf, size_t size) override { return text(buf, size, "SDK_SERIAL"); }
    CHIP_ERROR GetManufacturingDate(uint16_t &year, uint8_t &month, uint8_t &day) override {
        ++metadata_calls; year = 2026; month = 10; day = 23; return metadata_error;
    }
    CHIP_ERROR GetHardwareVersion(uint16_t &value) override {
        ++metadata_calls; value = 149; return metadata_error;
    }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &span) override {
        ++metadata_calls;
        if (metadata_error) return metadata_error;
        assert(span.size >= 2); span.data[0] = 0x12; span.data[1] = 0x34; span.size = 2;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetProductFinish(chip::app::Clusters::BasicInformation::ProductFinishEnum *finish) override {
        ++metadata_calls; *finish = chip::app::Clusters::BasicInformation::ProductFinishEnum::kMatte;
        return metadata_error;
    }
    CHIP_ERROR GetProductPrimaryColor(chip::app::Clusters::BasicInformation::ColorEnum *color) override {
        ++metadata_calls; *color = chip::app::Clusters::BasicInformation::ColorEnum::kBlue;
        return metadata_error;
    }
    CHIP_ERROR GetJointFabricMode(uint8_t &mode) override {
        ++metadata_calls; mode = 1; return metadata_error;
    }
};
static chip::DeviceLayer::DeviceInstanceInfoProvider *retained_provider;
static int original_inits, original_shutdowns;
static void original_init(uint16_t endpoint) {
    assert(endpoint == 0);
    // The SDK BasicInformation server keeps this reference after startup.
    retained_provider = chip::DeviceLayer::GetDeviceInstanceInfoProvider();
    assert(retained_provider); ++original_inits;
}
static void original_shutdown(uint16_t endpoint, chip::app::ClusterShutdownType type) {
    assert(endpoint == 0 && type == chip::app::ClusterShutdownType::kEndpointShutdown);
    ++original_shutdowns;
}

static void verify_metadata(chip::DeviceLayer::DeviceInstanceInfoProvider *info, SdkProvider &sdk) {
    const int before = sdk.metadata_calls;
    uint16_t value = 0;
    assert(info->GetVendorId(value) == CHIP_NO_ERROR && value == 0xfff1);
    assert(info->GetProductId(value) == CHIP_NO_ERROR && value == 0x8000);
    assert(info->GetHardwareVersion(value) == CHIP_NO_ERROR && value == 149);
    char text[48];
    assert(info->GetPartNumber(text, sizeof(text)) == CHIP_NO_ERROR && !std::strcmp(text, "SDK_PART"));
    assert(info->GetProductURL(text, sizeof(text)) == CHIP_NO_ERROR && !std::strcmp(text, "SDK_URL"));
    assert(info->GetProductLabel(text, sizeof(text)) == CHIP_NO_ERROR && !std::strcmp(text, "SDK_LABEL"));
    assert(info->GetSerialNumber(text, sizeof(text)) == CHIP_NO_ERROR && !std::strcmp(text, "SDK_SERIAL"));
    uint16_t year = 0; uint8_t month = 0, day = 0;
    assert(info->GetManufacturingDate(year, month, day) == CHIP_NO_ERROR);
    assert(year == 2026 && month == 10 && day == 23);
    uint8_t unique[4] = {}; chip::MutableByteSpan span{unique, sizeof(unique)};
    assert(info->GetRotatingDeviceIdUniqueId(span) == CHIP_NO_ERROR);
    assert(span.size == 2 && unique[0] == 0x12 && unique[1] == 0x34);
    chip::app::Clusters::BasicInformation::ProductFinishEnum finish;
    chip::app::Clusters::BasicInformation::ColorEnum color;
    assert(info->GetProductFinish(&finish) == CHIP_NO_ERROR);
    assert(finish == chip::app::Clusters::BasicInformation::ProductFinishEnum::kMatte);
    assert(info->GetProductPrimaryColor(&color) == CHIP_NO_ERROR);
    assert(color == chip::app::Clusters::BasicInformation::ColorEnum::kBlue);
    uint8_t mode = 0;
    assert(info->GetJointFabricMode(mode) == CHIP_NO_ERROR && mode == 1);
    assert(sdk.metadata_calls - before == 12 && sdk.name_calls == 0);
}

int main() {
    esp_matter::node_t node{};
    esp_matter::endpoint_t root{};
    esp_matter::cluster_t basic{original_init, original_shutdown};
    // Incomplete data models cannot silently bypass the SDK lifecycle hook.
    assert(install_instance_info_provider(nullptr) == ESP_ERR_INVALID_STATE);
    assert(install_instance_info_provider(&node) == ESP_ERR_INVALID_STATE);
    node.root = &root;
    assert(install_instance_info_provider(&node) == ESP_ERR_INVALID_STATE);
    root.basic = &basic; basic.init = nullptr;
    assert(install_instance_info_provider(&node) == ESP_ERR_INVALID_STATE);
    basic.init = original_init;
    esp_matter::callback_error = ESP_FAIL;
    assert(install_instance_info_provider(&node) == ESP_FAIL);
    assert(basic.init == original_init && basic.shutdown == original_shutdown);
    esp_matter::callback_error = 0;
    assert(install_instance_info_provider(&node) == ESP_OK);
    assert(basic.init != original_init && basic.shutdown == original_shutdown);
    const int installed = esp_matter::callback_writes;
    assert(install_instance_info_provider(&node) == ESP_OK);
    assert(esp_matter::callback_writes == installed); // do not wrap the wrapper
    assert(!chip::DeviceLayer::GetDeviceInstanceInfoProvider());

    SdkProvider sdk;
    // ESP-Matter start selects its provider AFTER the app installs the hook.
    chip::DeviceLayer::SetDeviceInstanceInfoProvider(&sdk);
    basic.init(0);
    assert(original_inits == 1 && retained_provider != &sdk);
    assert(retained_provider == chip::DeviceLayer::GetDeviceInstanceInfoProvider());
    char name[48];
    assert(retained_provider->GetVendorName(name, sizeof(name)) == CHIP_NO_ERROR && !std::strcmp(name, "M5Stack"));
    assert(retained_provider->GetProductName(name, sizeof(name)) == CHIP_NO_ERROR && !std::strcmp(name, "CoreS3 Thread BR"));
    assert(retained_provider->GetHardwareVersionString(name, sizeof(name)) == CHIP_NO_ERROR && !std::strcmp(name, "CoreS3 Thread BR K149"));
    assert(retained_provider->GetVendorName(name, 8) == CHIP_NO_ERROR && name[7] == '\0');
    assert(retained_provider->GetVendorName(name, 7) == CHIP_ERROR_BUFFER_TOO_SMALL);
    assert(retained_provider->GetVendorName(nullptr, sizeof(name)) == CHIP_ERROR_BUFFER_TOO_SMALL);
    assert(retained_provider->GetProductName(name, 0) == CHIP_ERROR_BUFFER_TOO_SMALL);
    verify_metadata(retained_provider, sdk);

    // Repeated init sees the app wrapper globally; it must keep the SDK fallback.
    basic.init(0);
    assert(original_inits == 2);
    verify_metadata(retained_provider, sdk);
    sdk.metadata_error = CHIP_ERROR_NOT_IMPLEMENTED;
    uint16_t vendor = 0;
    assert(retained_provider->GetVendorId(vendor) == CHIP_ERROR_NOT_IMPLEMENTED);
    assert(retained_provider->GetSerialNumber(name, sizeof(name)) == CHIP_ERROR_NOT_IMPLEMENTED);
    assert(retained_provider->GetVendorName(name, sizeof(name)) == CHIP_NO_ERROR);
    basic.shutdown(0, chip::app::ClusterShutdownType::kEndpointShutdown);
    assert(original_shutdowns == 1);
    std::puts("PASS production Matter identity: init-time provider capture, strings, metadata forwarding, callback lifecycle and errors");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = (ROOT / 'main/matter.cpp').read_text()
    start = source.index('constexpr char kVendorName[]')
    provider = source[start:source.index('/*\n * NodeLabel is', start)]
    startup = source[source.index('extern "C" esp_err_t matter_init(void)'):]
    assert startup.index('install_instance_info_provider(node)') < startup.index('esp_matter::start(')
    with tempfile.TemporaryDirectory(prefix='matter-identity-') as directory:
        temp = Path(directory)
        test = temp / 'identity.cpp'
        test.write_text(MOCKS + provider + TEST)
        compiler = [args.cc, 'c++'] if Path(args.cc).stem == 'zig' else [
            str(Path(args.cc).with_name('g++.exe' if args.cc.endswith('.exe') else 'g++'))]
        binary = temp / 'identity.exe'
        subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror',
                                  str(test), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
