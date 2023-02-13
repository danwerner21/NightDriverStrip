//+--------------------------------------------------------------------------
//
// File:        improvserial.cpp
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
//
// Description:
//
//   Wraps ImprovSerial to manage its state and provide an interface to it
//
// History:     Oct-9-2018         Davepl      File based on ESPHome GPLv3
//                     https://github.com/esphome/esphome/blob/dev/LICENSE
//
//---------------------------------------------------------------------------

#include "improvserial.h"
#include <WiFi.h>

static const char *const TAG = "improv_serial";

void ImprovSerial::setup(const String &firmware,
                         const String &version,
                         const String &variant,
                         const String &name,
                         HardwareSerial *serial)
{
    this->hw_serial_ = serial;
    this->firmware_name_ = firmware;
    this->firmware_version_ = version;
    this->hardware_variant_ = variant;
    this->device_name_ = name;

    if (WiFi.getMode() == WIFI_STA && WiFi.isConnected())
        this->state_ = improv::STATE_PROVISIONED;
    else
        this->state_ = improv::STATE_AUTHORIZED;

    ESP_LOGI(TAG, "Settings ssid=%s, password=%s", WiFi_ssid.c_str(), WiFi_password.c_str());
}

improv::State ImprovSerial::get_state()
{
    return this->state_;
}

String ImprovSerial::get_ssid()
{
    return String(this->command_.ssid.c_str());
}

String ImprovSerial::get_password()
{
    return String(this->command_.password.c_str());
}

int ImprovSerial::available_()
{
    return this->hw_serial_->available();
}

uint8_t ImprovSerial::read_byte_()
{
    uint8_t data;
    this->hw_serial_->readBytes(&data, 1);
    return data;
}

void ImprovSerial::write_data_(std::vector<uint8_t> &data)
{
    data.push_back('\n');
    this->hw_serial_->write(data.data(), data.size());
}

bool ImprovSerial::loop(bool timeout)
{
    const uint32_t now = millis();
    if (now - this->last_read_byte_ > 50)
    {
        this->rx_buffer_.clear();
        this->last_read_byte_ = now;
    }
    while (this->available_())
    {
        uint8_t byte = this->read_byte_();
        if (this->parse_improv_serial_byte_(byte))
        {
            this->last_read_byte_ = now;
        }
        else
        {
            this->rx_buffer_.clear();
        }
    }
    if (this->state_ == improv::STATE_PROVISIONING)
    {
        if (WiFi.getMode() == WIFI_AP || (WiFi.getMode() == WIFI_STA && WiFi.isConnected()))
        {
            this->set_state_(improv::STATE_PROVISIONED);
            std::vector<uint8_t> url = this->build_rpc_settings_response_(improv::WIFI_SETTINGS);
            this->send_response_(url);
            return true;
        }
        else if (timeout)
        {
            this->on_wifi_connect_timeout_();
        }
    }
    return false;
}

std::vector<uint8_t> ImprovSerial::build_rpc_settings_response_(improv::Command command)
{
    std::vector<String> urls;

    String webserver_url = String("http://") + WiFi.localIP().toString();
    urls.push_back(webserver_url);
    std::vector<uint8_t> data = improv::build_rpc_response(command, urls, false);

    return data;
}

std::vector<uint8_t> ImprovSerial::build_version_info_()
{
    std::vector<String> infos = {this->firmware_name_, this->firmware_version_, this->hardware_variant_, this->device_name_};
    std::vector<uint8_t> data = improv::build_rpc_response(improv::GET_DEVICE_INFO, infos, false);
    return data;
};

bool ImprovSerial::parse_improv_serial_byte_(uint8_t byte)
{
    size_t at = this->rx_buffer_.size();
    this->rx_buffer_.push_back(byte);
    ESP_LOGD(TAG, "Improv Serial byte: 0x%02X", byte);

    // Checks the bytestream to see if we're still seeing what looks like the IMPROV header
    // There are many more elegant and less readable ways to do this, but... let's keep it simple.

    const uint8_t *raw = &this->rx_buffer_[0];
    if (at == 0)
        return byte == 'I';
    if (at == 1)
        return byte == 'M';
    if (at == 2)
        return byte == 'P';
    if (at == 3)
        return byte == 'R';
    if (at == 4)
        return byte == 'O';
    if (at == 5)
        return byte == 'V';

    if (at == 6)
        return byte == IMPROV_SERIAL_VERSION;

    if (at == 7)
        return true;

    uint8_t type = raw[7];

    if (at == 8)
        return true;

    uint8_t data_len = raw[8];

    if (at < 8 + data_len)
        return true;

    if (at == 8 + data_len)
        return true;

    // THe last byte of the packet needs to be a checksum, so compute it and stuff it in

    if (at == 8 + data_len + 1)
    {
        uint8_t checksum = 0x00;
        for (uint8_t i = 0; i < at; i++)
            checksum += raw[i];

        if (checksum != byte)
        {
            ESP_LOGW(TAG, "Error decoding Improv payload");
            this->set_error_(improv::ERROR_INVALID_RPC);
            return false;
        }

        if (type == TYPE_RPC)
        {
            this->set_error_(improv::ERROR_NONE);
            auto command = improv::parse_improv_data(&raw[9], data_len, false);
            return this->parse_improv_payload_(command);
        }
    }

    // If we got here then the command coming is improv, but not an RPC command

    return false;
}

bool ImprovSerial::parse_improv_payload_(improv::ImprovCommand &command)
{
    switch (command.command)
    {
        // When a "Set the WiFi" RPC call comes in we save the credentials to
        //   NVS, then disconnect and reconnect the WiFi using whatever creds
        //   were supplied.  Returns before the connection is complete, as it
        //   sets the state to PROVISIONING so the remote caller can check
        //   back on the status to see progress

        case improv::WIFI_SETTINGS:
        {
            WiFi_ssid = command.ssid.c_str();
            WiFi_password = command.password.c_str();

            if (!WriteWiFiConfig())
                Serial.print("Failed writing WiFi config to NVS");

            this->set_state_(improv::STATE_PROVISIONING);

            ESP_LOGD(TAG, "Received Improv wifi settings ssid=%s, password=%s", command.ssid.c_str(), "******");

            WiFi.disconnect();
            WiFi.mode(WIFI_STA);
            WiFi.begin(WiFi_ssid.c_str(), WiFi_password.c_str());

            this->command_.command = command.command;
            this->command_.ssid = command.ssid;
            this->command_.password = command.password;

            return true;
        }

        // Return the current state of the WiFi setup:  authorized, provisioning, or provisioned

        case improv::GET_CURRENT_STATE:
        {
            this->set_state_(this->state_);
            if (this->state_ == improv::STATE_PROVISIONED)
            {
                std::vector<uint8_t> url = this->build_rpc_settings_response_(improv::GET_CURRENT_STATE);
                this->send_response_(url);
            }
            return true;
        }

        // Return info about the ESP32 itself

        case improv::GET_DEVICE_INFO:
        {
            std::vector<uint8_t> info = this->build_version_info_();
            this->send_response_(info);
            return true;
        }

        default:
        {
            ESP_LOGW(TAG, "Unknown Improv payload");
            this->set_error_(improv::ERROR_UNKNOWN_RPC);
            return false;
        }
    }
}

// Allows the remote caller to set the provisioning state

void ImprovSerial::set_state_(improv::State state)
{
    this->state_ = state;

    std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(11);
    data[6] = IMPROV_SERIAL_VERSION;
    data[7] = TYPE_CURRENT_STATE;
    data[8] = 1;
    data[9] = state;

    uint8_t checksum = 0x00;
    for (uint8_t d : data)
        checksum += d;
    data[10] = checksum;

    this->write_data_(data);
}

// Allows the caller to inform us that an error has occured

void ImprovSerial::set_error_(improv::Error error)
{
    std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(11);
    data[6] = IMPROV_SERIAL_VERSION;
    data[7] = TYPE_ERROR_STATE;
    data[8] = 1;
    data[9] = error;

    Serial.printf("Improv Serial received an error condition from the caller: %d", error);

    uint8_t checksum = 0x00;
    for (uint8_t d : data)
        checksum += d;
    data[10] = checksum;
    this->write_data_(data);
}

void ImprovSerial::send_response_(std::vector<uint8_t> &response)
{
    std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
    data.resize(9);
    data[6] = IMPROV_SERIAL_VERSION;
    data[7] = TYPE_RPC_RESPONSE;
    data[8] = response.size();
    data.insert(data.end(), response.begin(), response.end());

    uint8_t checksum = 0x00;
    for (uint8_t d : data)
        checksum += d;
    data.push_back(checksum);

    this->write_data_(data);
}

void ImprovSerial::on_wifi_connect_timeout_()
{
    this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
    this->set_state_(improv::STATE_AUTHORIZED);
    ESP_LOGW(TAG, "Timed out trying to connect to given WiFi network");
    WiFi.disconnect();
}

// The one and only instance of ImprovSerial

ImprovSerial g_ImprovSerial; 

