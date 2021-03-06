#include "scout_base/scout_base.hpp"

#include <string>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ratio>
#include <thread>

namespace
{
// source: https://github.com/rxdu/stopwatch
struct StopWatch
{
    using Clock = std::chrono::high_resolution_clock;
    using time_point = typename Clock::time_point;
    using duration = typename Clock::duration;

    StopWatch() { tic_point = Clock::now(); };

    time_point tic_point;

    void tic()
    {
        tic_point = Clock::now();
    };

    double toc()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tic_point).count() / 1000000.0;
    };

    // for different precisions
    double stoc()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - tic_point).count();
    };

    double mtoc()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - tic_point).count();
    };

    double utoc()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tic_point).count();
    };

    double ntoc()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tic_point).count();
    };

    // you have to call tic() before calling this function
    void sleep_until_ms(int64_t period_ms)
    {
        int64_t duration = period_ms - std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - tic_point).count();

        if (duration > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    };

    void sleep_until_us(int64_t period_us)
    {
        int64_t duration = period_us - std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - tic_point).count();

        if (duration > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(duration));
    };
};
} // namespace

namespace wescore
{
ScoutBase::~ScoutBase()
{
    if (serial_connected_)
        serial_if_->close();

    if (cmd_thread_.joinable())
        cmd_thread_.join();
}

void ScoutBase::Connect(std::string dev_name, int32_t baud_rate)
{
    if (baud_rate == 0)
    {
        ConfigureCANBus(dev_name);
    }
    else
    {
        ConfigureSerial(dev_name, baud_rate);

        if (!serial_connected_)
            std::cerr << "ERROR: Failed to connect to serial port" << std::endl;
    }
}

void ScoutBase::Disconnect()
{
    if (serial_connected_)
    {
        if (serial_if_->is_open())
            serial_if_->close();
    }
}

void ScoutBase::ConfigureCANBus(const std::string &can_if_name)
{
    can_if_ = std::make_shared<ASyncCAN>(can_if_name);

    can_if_->set_receive_callback(std::bind(&ScoutBase::ParseCANFrame, this, std::placeholders::_1));

    can_connected_ = true;
}

void ScoutBase::ConfigureSerial(const std::string uart_name, int32_t baud_rate)
{
    serial_if_ = std::make_shared<ASyncSerial>(uart_name, baud_rate);
    serial_if_->open();

    if (serial_if_->is_open())
        serial_connected_ = true;

    serial_if_->set_receive_callback(std::bind(&ScoutBase::ParseUARTBuffer, this,
                                               std::placeholders::_1,
                                               std::placeholders::_2,
                                               std::placeholders::_3));

    serial_parser_.SetReceiveCallback(std::bind(&ScoutBase::NewStatusMsgReceivedCallback, this, std::placeholders::_1));
}

void ScoutBase::StartCmdThread()
{
    cmd_thread_ = std::thread(std::bind(&ScoutBase::ControlLoop, this, cmd_thread_period_ms_));
    cmd_thread_started_ = true;
}

void ScoutBase::SendMotionCmd(uint8_t count)
{
    // motion control message
    MotionControlMessage m_msg;

    if (can_connected_)
    {
        m_msg.id = ScoutCANParser::CAN_MSG_MOTION_CONTROL_CMD_ID;
        m_msg.msg.cmd.control_mode = CTRL_MODE_CMD_CAN;
    }
    else if (serial_connected_)
    {
        m_msg.id = ScoutSerialParser::FRAME_MOTION_CONTROL_CMD_ID;
        m_msg.msg.cmd.control_mode = CTRL_MODE_CMD_UART;
    }

    motion_cmd_mutex_.lock();
    m_msg.msg.cmd.fault_clear_flag = static_cast<uint8_t>(current_motion_cmd_.fault_clear_flag);
    m_msg.msg.cmd.linear_velocity_cmd = current_motion_cmd_.linear_velocity;
    m_msg.msg.cmd.angular_velocity_cmd = current_motion_cmd_.angular_velocity;
    motion_cmd_mutex_.unlock();

    m_msg.msg.cmd.reserved0 = 0;
    m_msg.msg.cmd.reserved1 = 0;
    m_msg.msg.cmd.count = count;

    if (can_connected_)
        m_msg.msg.cmd.checksum = ScoutCANParser::Agilex_CANMsgChecksum(m_msg.id, m_msg.msg.raw, m_msg.len);
    // serial_connected_: checksum will be calculated later when packed into a complete serial frame

    if (can_connected_)
    {
        // send to can bus
        can_frame m_frame = ScoutCANParser::PackMsgToScoutCANFrame(m_msg);
        can_if_->send_frame(m_frame);
    }
    else
    {
        // send to serial port
        ScoutSerialParser::PackMotionControlMsgToBuffer(m_msg, tx_buffer_, tx_cmd_len_);
        serial_if_->send_bytes(tx_buffer_, tx_cmd_len_);
    }
}

void ScoutBase::SendLightCmd(uint8_t count)
{
    LightControlMessage l_msg;

    if (can_connected_)
        l_msg.id = ScoutCANParser::CAN_MSG_LIGHT_CONTROL_CMD_ID;
    else if (serial_connected_)
        l_msg.id = ScoutSerialParser::FRAME_LIGHT_CONTROL_CMD_ID;

    light_cmd_mutex_.lock();
    if (light_ctrl_enabled_)
    {
        l_msg.msg.cmd.light_ctrl_enable = LIGHT_ENABLE_CTRL;

        l_msg.msg.cmd.front_light_mode = static_cast<uint8_t>(current_light_cmd_.front_mode);
        l_msg.msg.cmd.front_light_custom = current_light_cmd_.front_custom_value;
        l_msg.msg.cmd.rear_light_mode = static_cast<uint8_t>(current_light_cmd_.rear_mode);
        l_msg.msg.cmd.rear_light_custom = current_light_cmd_.rear_custom_value;
    }
    else
    {
        l_msg.msg.cmd.light_ctrl_enable = LIGHT_DISABLE_CTRL;

        l_msg.msg.cmd.front_light_mode = LIGHT_MODE_CONST_OFF;
        l_msg.msg.cmd.front_light_custom = 0;
        l_msg.msg.cmd.rear_light_mode = LIGHT_MODE_CONST_OFF;
        l_msg.msg.cmd.rear_light_custom = 0;
    }
    light_ctrl_requested_ = false;
    light_cmd_mutex_.unlock();

    l_msg.msg.cmd.reserved0 = 0;
    l_msg.msg.cmd.count = count;

    if (can_connected_)
        l_msg.msg.cmd.checksum = ScoutCANParser::Agilex_CANMsgChecksum(l_msg.id, l_msg.msg.raw, l_msg.len);
    // serial_connected_: checksum will be calculated later when packed into a complete serial frame

    if (can_connected_)
    {
        // send to can bus
        can_frame l_frame = ScoutCANParser::PackMsgToScoutCANFrame(l_msg);
        can_if_->send_frame(l_frame);
    }
    else
    {
        // send to serial port
        ScoutSerialParser::PackLightControlMsgToBuffer(l_msg, tx_buffer_, tx_cmd_len_);
        serial_if_->send_bytes(tx_buffer_, tx_cmd_len_);
    }
}

void ScoutBase::ControlLoop(int32_t period_ms)
{
    StopWatch ctrl_sw;
    uint8_t cmd_count = 0;
    uint8_t light_cmd_count = 0;
    while (true)
    {
        ctrl_sw.tic();

        // motion control message
        SendMotionCmd(cmd_count++);

        // check if there is request for light control
        if (light_ctrl_requested_)
            SendLightCmd(light_cmd_count++);

        ctrl_sw.sleep_until_ms(period_ms);
        // std::cout << "control loop update frequency: " << 1.0 / ctrl_sw.toc() << std::endl;
    }
}

ScoutState ScoutBase::GetScoutState()
{
    std::lock_guard<std::mutex> guard(scout_state_mutex_);
    return scout_state_;
}

void ScoutBase::SetMotionCommand(double linear_vel, double angular_vel, ScoutMotionCmd::FaultClearFlag fault_clr_flag)
{
    // make sure cmd thread is started before attempting to send commands
    if (!cmd_thread_started_)
        StartCmdThread();

    if (linear_vel < ScoutMotionCmd::min_linear_velocity)
        linear_vel = ScoutMotionCmd::min_linear_velocity;
    if (linear_vel > ScoutMotionCmd::max_linear_velocity)
        linear_vel = ScoutMotionCmd::max_linear_velocity;
    if (angular_vel < ScoutMotionCmd::min_angular_velocity)
        angular_vel = ScoutMotionCmd::min_angular_velocity;
    if (angular_vel > ScoutMotionCmd::max_angular_velocity)
        angular_vel = ScoutMotionCmd::max_angular_velocity;

    std::lock_guard<std::mutex> guard(motion_cmd_mutex_);
    current_motion_cmd_.linear_velocity = static_cast<uint8_t>(linear_vel / ScoutMotionCmd::max_linear_velocity * 100.0);
    current_motion_cmd_.angular_velocity = static_cast<uint8_t>(angular_vel / ScoutMotionCmd::max_angular_velocity * 100.0);
    current_motion_cmd_.fault_clear_flag = fault_clr_flag;
}

void ScoutBase::SetLightCommand(ScoutLightCmd cmd)
{
    std::lock_guard<std::mutex> guard(light_cmd_mutex_);
    current_light_cmd_ = cmd;
    light_ctrl_enabled_ = true;
    light_ctrl_requested_ = true;
}

void ScoutBase::DisableLightCmdControl()
{
    std::lock_guard<std::mutex> guard(light_cmd_mutex_);
    light_ctrl_enabled_ = false;
    light_ctrl_requested_ = true;
}

void ScoutBase::ParseCANFrame(can_frame *rx_frame)
{
    // validate checksum, discard frame if fails
    if (!rx_frame->data[7] == ScoutCANParser::Agilex_CANMsgChecksum(rx_frame->can_id, rx_frame->data, rx_frame->can_dlc))
    {
        std::cerr << "ERROR: checksum mismatch, discard frame with id " << rx_frame->can_id << std::endl;
        return;
    }

    // otherwise, update robot state with new frame
    auto status_msg = ScoutCANParser::UnpackScoutCANFrameToMsg(rx_frame);
    NewStatusMsgReceivedCallback(status_msg);
}

void ScoutBase::ParseUARTBuffer(uint8_t *buf, const size_t bufsize, size_t bytes_received)
{
    // std::cout << "bytes received from serial: " << bytes_received << std::endl;
    // serial_parser_.PrintStatistics();
    serial_parser_.ParseBuffer(buf, bytes_received);
}

void ScoutBase::NewStatusMsgReceivedCallback(const ScoutStatusMessage &msg)
{
    // std::cout << "new status msg received" << std::endl;
    std::lock_guard<std::mutex> guard(scout_state_mutex_);
    UpdateScoutState(msg, scout_state_);
}

void ScoutBase::UpdateScoutState(const ScoutStatusMessage &status_msg, ScoutState &state)
{
    switch (status_msg.updated_msg_type)
    {
    case ScoutMotionStatusMsg:
    {
        // std::cout << "motion control feedback received" << std::endl;
        const MotionStatusMessage &msg = status_msg.motion_status_msg;
        state.linear_velocity = static_cast<int16_t>(static_cast<uint16_t>(msg.msg.status.linear_velocity.low_byte) | static_cast<uint16_t>(msg.msg.status.linear_velocity.high_byte) << 8) / 1000.0;
        state.angular_velocity = static_cast<int16_t>(static_cast<uint16_t>(msg.msg.status.angular_velocity.low_byte) | static_cast<uint16_t>(msg.msg.status.angular_velocity.high_byte) << 8) / 1000.0;
        break;
    }
    case ScoutLightStatusMsg:
    {
        // std::cout << "light control feedback received" << std::endl;
        const LightStatusMessage &msg = status_msg.light_status_msg;
        if (msg.msg.status.light_ctrl_enable == LIGHT_DISABLE_CTRL)
            state.light_control_enabled = false;
        else
            state.light_control_enabled = true;
        state.front_light_state.mode = msg.msg.status.front_light_mode;
        state.front_light_state.custom_value = msg.msg.status.front_light_custom;
        state.rear_light_state.mode = msg.msg.status.rear_light_mode;
        state.rear_light_state.custom_value = msg.msg.status.rear_light_custom;
        break;
    }
    case ScoutSystemStatusMsg:
    {
        // std::cout << "system status feedback received" << std::endl;
        const SystemStatusMessage &msg = status_msg.system_status_msg;
        state.control_mode = msg.msg.status.control_mode;
        state.base_state = msg.msg.status.base_state;
        state.battery_voltage = (static_cast<uint16_t>(msg.msg.status.battery_voltage.low_byte) | static_cast<uint16_t>(msg.msg.status.battery_voltage.high_byte) << 8) / 10.0;
        state.fault_code = (static_cast<uint16_t>(msg.msg.status.fault_code.low_byte) | static_cast<uint16_t>(msg.msg.status.fault_code.high_byte) << 8);
        break;
    }
    case ScoutMotor1DriverStatusMsg:
    {
        // std::cout << "motor 1 driver feedback received" << std::endl;
        const MotorDriverStatusMessage &msg = status_msg.motor_driver_status_msg;
        state.motor_states[0].current = (static_cast<uint16_t>(msg.msg.status.current.low_byte) | static_cast<uint16_t>(msg.msg.status.current.high_byte) << 8) / 10.0;
        state.motor_states[0].rpm = static_cast<int16_t>(static_cast<uint16_t>(msg.msg.status.rpm.low_byte) | static_cast<uint16_t>(msg.msg.status.rpm.high_byte) << 8);
        state.motor_states[0].temperature = msg.msg.status.temperature;
        break;
    }
    case ScoutMotor2DriverStatusMsg:
    {
        // std::cout << "motor 2 driver feedback received" << std::endl;
        const MotorDriverStatusMessage &msg = status_msg.motor_driver_status_msg;
        state.motor_states[1].current = (static_cast<uint16_t>(msg.msg.status.current.low_byte) | static_cast<uint16_t>(msg.msg.status.current.high_byte) << 8) / 10.0;
        state.motor_states[1].rpm = static_cast<int16_t>(static_cast<uint16_t>(msg.msg.status.rpm.low_byte) | static_cast<uint16_t>(msg.msg.status.rpm.high_byte) << 8);
        state.motor_states[1].temperature = msg.msg.status.temperature;
        break;
    }
    case ScoutMotor3DriverStatusMsg:
    {
        // std::cout << "motor 3 driver feedback received" << std::endl;
        const MotorDriverStatusMessage &msg = status_msg.motor_driver_status_msg;
        state.motor_states[2].current = (static_cast<uint16_t>(msg.msg.status.current.low_byte) | static_cast<uint16_t>(msg.msg.status.current.high_byte) << 8) / 10.0;
        state.motor_states[2].rpm = static_cast<int16_t>(static_cast<uint16_t>(msg.msg.status.rpm.low_byte) | static_cast<uint16_t>(msg.msg.status.rpm.high_byte) << 8);
        state.motor_states[2].temperature = msg.msg.status.temperature;
        break;
    }
    case ScoutMotor4DriverStatusMsg:
    {
        // std::cout << "motor 4 driver feedback received" << std::endl;
        const MotorDriverStatusMessage &msg = status_msg.motor_driver_status_msg;
        state.motor_states[3].current = (static_cast<uint16_t>(msg.msg.status.current.low_byte) | static_cast<uint16_t>(msg.msg.status.current.high_byte) << 8) / 10.0;
        state.motor_states[3].rpm = static_cast<int16_t>(static_cast<uint16_t>(msg.msg.status.rpm.low_byte) | static_cast<uint16_t>(msg.msg.status.rpm.high_byte) << 8);
        state.motor_states[3].temperature = msg.msg.status.temperature;
        break;
    }
    }
}
} // namespace wescore
