#include "eth.hpp"

#include <string.h>

#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_phy_lan87xx.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"

static const char *TAG = "eth";
static EventGroupHandle_t events;
static const EventBits_t GOT_IP = BIT0;
static volatile uint32_t link_drops = 0;
static volatile bool link_was_up = false;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED)
    {
        ESP_LOGI(TAG, "link up");
        link_was_up = true;
    }
    else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED)
    {
        ESP_LOGW(TAG, "link down");
        if (link_was_up)
        {
            link_drops = link_drops + 1;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP)
    {
        const ip_event_got_ip_t *ip = static_cast<const ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "address " IPSTR ", gateway " IPSTR, IP2STR(&ip->ip_info.ip), IP2STR(&ip->ip_info.gw));
        xEventGroupSetBits(events, GOT_IP);
    }
}

esp_err_t eth_start(const eth_config &config, int timeout_ms)
{
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);

    // The ESP32-POE switches the LAN8720 on through GPIO12
    gpio_reset_pin((gpio_num_t)config.power_gpio);
    gpio_set_direction((gpio_num_t)config.power_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)config.power_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = config.mdc_gpio;
    emac_cfg.smi_gpio.mdio_num = config.mdio_gpio;
    // The ESP32 generates the 50 MHz RMII clock: the board has no oscillator for it. ESP-IDF warns
    // this can be unstable with Wi-Fi on (ESP32 errata); see #18.
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac_cfg.clock_config.rmii.clock_gpio = config.clock_out_gpio;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = config.phy_addr;
    phy_cfg.reset_gpio_num = -1;  // no reset line; power is switched instead
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
    if (mac == nullptr || phy == nullptr)
    {
        ESP_LOGE(TAG, "Ethernet MAC or PHY driver failed to start");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = nullptr;
    esp_err_t err = esp_eth_driver_install(&eth_cfg, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Ethernet driver: %s (is the PHY powered and at address %d?)", esp_err_to_name(err),
                 config.phy_addr);
        return err;
    }
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(handle)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(handle));

    if (!(xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP))
    {
        ESP_LOGE(TAG, "no address from DHCP within %d ms: is the cable in?", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

uint32_t eth_link_drops(void)
{
    return link_drops;
}

esp_err_t udp_out::open(const char *host, uint16_t port)
{
    static_assert(sizeof(addr) >= sizeof(sockaddr_in), "address storage");
    sockaddr_in *a = reinterpret_cast<sockaddr_in *>(addr);
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a->sin_addr) != 1)
    {
        ESP_LOGE(TAG, "not an IPv4 address: '%s'", host);
        return ESP_ERR_INVALID_ARG;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sending payloads to %s:%u", host, port);
    return ESP_OK;
}

void udp_out::send_payload(const uint8_t *buf, size_t size)
{
    if (fd >= 0 && sendto(fd, buf, size, 0, reinterpret_cast<sockaddr *>(addr), sizeof(sockaddr_in)) == (int)size)
    {
        sent += 1;
    }
    else
    {
        failed += 1;
    }
}
