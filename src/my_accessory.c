/*
 * my_accessory.c
 * HomeKit 恒温器 accessory（空调红外控制器）
 */
#include <homekit/homekit.h>
#include <homekit/characteristics.h>

void my_accessory_identify(homekit_value_t _value) {
	printf("accessory identify\n");
}

// 恒温器特性（setter 在 main.cpp 中挂接）
homekit_characteristic_t cha_current_heating_cooling_state = HOMEKIT_CHARACTERISTIC_(CURRENT_HEATING_COOLING_STATE, 0);
homekit_characteristic_t cha_target_heating_cooling_state  = HOMEKIT_CHARACTERISTIC_(TARGET_HEATING_COOLING_STATE, 0);
homekit_characteristic_t cha_current_temperature           = HOMEKIT_CHARACTERISTIC_(CURRENT_TEMPERATURE, 25);
homekit_characteristic_t cha_target_temperature            = HOMEKIT_CHARACTERISTIC_(TARGET_TEMPERATURE, 26, .min_step = (float[]) {1});
homekit_characteristic_t cha_temperature_display_units     = HOMEKIT_CHARACTERISTIC_(TEMPERATURE_DISPLAY_UNITS, 0);
homekit_characteristic_t cha_current_relative_humidity     = HOMEKIT_CHARACTERISTIC_(CURRENT_RELATIVE_HUMIDITY, 50);
homekit_characteristic_t cha_name                          = HOMEKIT_CHARACTERISTIC_(NAME, "空调控制器");
homekit_characteristic_t cha_fan_active                    = HOMEKIT_CHARACTERISTIC_(ACTIVE, 1);
homekit_characteristic_t cha_fan_rotation_speed            = HOMEKIT_CHARACTERISTIC_(ROTATION_SPEED, 50);

homekit_accessory_t *accessories[] = {
	HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_thermostat, .services=(homekit_service_t*[]) {
		HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
			HOMEKIT_CHARACTERISTIC(NAME, "空调控制器"),
			HOMEKIT_CHARACTERISTIC(MANUFACTURER, "ESP8266 HomeKit"),
			HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "8266AC01"),
			HOMEKIT_CHARACTERISTIC(MODEL, "AC-IR-CONTROLLER"),
			HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "1.0"),
			HOMEKIT_CHARACTERISTIC(IDENTIFY, my_accessory_identify),
			NULL
		}),
		HOMEKIT_SERVICE(THERMOSTAT, .primary=true, .characteristics=(homekit_characteristic_t*[]) {
			&cha_current_heating_cooling_state,
			&cha_target_heating_cooling_state,
			&cha_current_temperature,
			&cha_target_temperature,
			&cha_temperature_display_units,
			&cha_current_relative_humidity,
			&cha_name,
			NULL
		}),
		HOMEKIT_SERVICE(FAN, .primary=false, .characteristics=(homekit_characteristic_t*[]) {
			&cha_fan_active,
			&cha_fan_rotation_speed,
			HOMEKIT_CHARACTERISTIC(NAME, "空调风速"),
			NULL
		}),
		NULL
	}),
	NULL
};

homekit_server_config_t config = {
	.accessories = accessories,
	.password = "111-11-111",
	.setupId = "AC01",
	.config_number = 2
};
