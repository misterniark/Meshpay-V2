#include "meshpay/device_hal.h"

/* Driver de la carte LILYGO T-Deck / T-Deck Plus (carte fondateur).
 * Pour l'instant : uniquement le décodage clavier pur (testable hors banc).
 * Les drivers matériels (écran ST7789, tactile GT911, clavier I2C, LoRa,
 * batterie) seront ajoutés en Phase 2 du Palier 0. */

/* Le clavier T-Deck (ESP32-C3 @0x55) renvoie 0 quand aucune touche n'est
 * pressée, sinon le code ASCII de la touche. Décodage pur, testable hors banc. */
esp_err_t meshpay_hal_tdeck_keyboard_decode(uint8_t raw, bool *has_key, char *ch)
{
    if (has_key == NULL || ch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (raw == 0) {
        *has_key = false;
        *ch = 0;
        return ESP_OK;
    }
    *has_key = true;
    *ch = (char)raw;
    return ESP_OK;
}
