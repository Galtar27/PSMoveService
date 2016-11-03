//-- includes -----
#include "MorpheusHMD.h"
#include "DeviceInterface.h"
#include "DeviceManager.h"
#include "HMDDeviceEnumerator.h"
#include "MathUtility.h"
#include "ServerLog.h"
#include "ServerUtility.h"
#include "hidapi.h"
#include <vector>
#include <cstdlib>
#ifdef _WIN32
#define _USE_MATH_DEFINES
#endif
#include <math.h>

//-- constants -----
#define MORPHEUS_SENSOR_INTERFACE 4
#define MORPHEUS_COMMAND_INTERFACE 5

#define MORPHEUS_HMD_STATE_BUFFER_MAX 4
#define METERS_TO_CENTIMETERS 100

// -- private definitions -----
class MorpheusHIDDetails 
{
public:
	std::string device_identifier;
    std::string sensor_device_path;
	hid_device *sensor_device_handle;
	std::string command_device_path;
	hid_device *command_device_handle;

    MorpheusHIDDetails()
    {
        Reset();
    }

    void Reset()
    {
		device_identifier = "";
        sensor_device_path = "";
		sensor_device_handle = nullptr;
		command_device_path = "";
		command_device_handle = nullptr;
    }
};

enum eMorpheusButton : unsigned char
{
	VolumePlus = 2,
	VolumeMinus = 4,
	MicrophoneMute = 8,
};

struct MorpheusRawSensorFrame
{
	unsigned char accel_x[2];
	unsigned char accel_y[2];
	unsigned char accel_z[2];
	unsigned char gyro_x[2];
	unsigned char gyro_y[2];
	unsigned char gyro_z[2];
};

#pragma pack(1)
struct MorpheusDataInput
{
	eMorpheusButton buttons;					// byte 0
	unsigned char unk0;							// byte 1
	unsigned char volume;                       // byte 2
	unsigned char unk1[5];                      // byte 3-7

	union
	{
		unsigned char asByte;
		struct
		{
			unsigned char hmdOnHead : 1;
			unsigned char displayIsOn : 1;
			unsigned char HDMIDisconnected : 1;
			unsigned char microphoneMuted : 1;
			unsigned char headphonesPresent : 1;
			unsigned char unk1 : 2;
			unsigned char timer : 1;
		};
	} headsetFlags;								// byte 8

	unsigned char unkFlags;     				// byte 9
	unsigned char unk2[4];						// byte 10-17

	unsigned short frame;						// byte 18-19
	MorpheusRawSensorFrame imu_frame_0;         // byte 20-31
	unsigned char unk3[4];						// byte 32-35
	MorpheusRawSensorFrame imu_frame_1;         // byte 36-47

    MorpheusDataInput()
    {
        Reset();
    }

    void Reset()
    {
        memset(this, 0, sizeof(MorpheusDataInput));
    }
};
#pragma pack()

// -- public methods

// -- Morpheus HMD Config
const int MorpheusHMDConfig::CONFIG_VERSION = 1;

const boost::property_tree::ptree
MorpheusHMDConfig::config2ptree()
{
    boost::property_tree::ptree pt;

	pt.put("is_valid", is_valid);
	pt.put("version", MorpheusHMDConfig::CONFIG_VERSION);

	pt.put("Calibration.Accel.Gain", accelerometer_gain);
	pt.put("Calibration.Accel.Variance", accelerometer_variance);
	pt.put("Calibration.Gyro.Gain", gyro_gain);
	pt.put("Calibration.Gyro.Variance", gyro_variance);
	pt.put("Calibration.Gyro.Drift", gyro_drift);
	pt.put("Calibration.Identity.Gravity.X", identity_gravity_direction.i);
	pt.put("Calibration.Identity.Gravity.Y", identity_gravity_direction.j);
	pt.put("Calibration.Identity.Gravity.Z", identity_gravity_direction.k);

	pt.put("OrientationFilter.MinQualityScreenArea", min_orientation_quality_screen_area);
	pt.put("OrientationFilter.MaxQualityScreenArea", max_orientation_quality_screen_area);

	pt.put("PositionFilter.MinQualityScreenArea", min_position_quality_screen_area);
	pt.put("PositionFilter.MaxQualityScreenArea", max_position_quality_screen_area);

	pt.put("PositionFilter.MaxVelocity", max_velocity);

	pt.put("prediction_time", prediction_time);
	pt.put("max_poll_failure_count", max_poll_failure_count);

	writeTrackingColor(pt, tracking_color_id);

    return pt;
}

void
MorpheusHMDConfig::ptree2config(const boost::property_tree::ptree &pt)
{
    version = pt.get<int>("version", 0);

    if (version == MorpheusHMDConfig::CONFIG_VERSION)
    {
		is_valid = pt.get<bool>("is_valid", false);
		prediction_time = pt.get<float>("prediction_time", 0.f);
		max_poll_failure_count = pt.get<long>("max_poll_failure_count", 100);

		// Use the current accelerometer values (constructor defaults) as the default values
		accelerometer_gain = pt.get<float>("Calibration.Accel.Gain", accelerometer_gain);
		accelerometer_variance = pt.get<float>("Calibration.Accel.Variance", accelerometer_variance);

		// Use the current gyroscope values (constructor defaults) as the default values
		gyro_gain = pt.get<float>("Calibration.Gyro.Gain", gyro_gain);
		gyro_variance = pt.get<float>("Calibration.Gyro.Variance", gyro_variance);
		gyro_drift = pt.get<float>("Calibration.Gyro.Drift", gyro_drift);

		// Get the orientation filter parameters
		min_orientation_quality_screen_area = pt.get<float>("OrientationFilter.MinQualityScreenArea", min_orientation_quality_screen_area);
		max_orientation_quality_screen_area = pt.get<float>("OrientationFilter.MaxQualityScreenArea", max_orientation_quality_screen_area);

		// Get the position filter parameters
		min_position_quality_screen_area = pt.get<float>("PositionFilter.MinQualityScreenArea", min_position_quality_screen_area);
		max_position_quality_screen_area = pt.get<float>("PositionFilter.MaxQualityScreenArea", max_position_quality_screen_area);
		max_velocity = pt.get<float>("PositionFilter.MaxVelocity", max_velocity);

		// Get the calibration direction for "down"
		identity_gravity_direction.i = pt.get<float>("Calibration.Identity.Gravity.X", identity_gravity_direction.i);
		identity_gravity_direction.j = pt.get<float>("Calibration.Identity.Gravity.Y", identity_gravity_direction.j);
		identity_gravity_direction.k = pt.get<float>("Calibration.Identity.Gravity.Z", identity_gravity_direction.k);

		// Read the tracking color
		tracking_color_id = static_cast<eCommonTrackingColorID>(readTrackingColor(pt));
    }
    else
    {
        SERVER_LOG_WARNING("MorpheusHMDConfig") <<
            "Config version " << version << " does not match expected version " <<
            MorpheusHMDConfig::CONFIG_VERSION << ", Using defaults.";
    }
}

// -- Morpheus HMD Sensor Frame -----
void MorpheusHMDSensorFrame::parse_data_input(
	const MorpheusHMDConfig *config,
	const MorpheusRawSensorFrame *data_input)
{
	// Piece together the 16-bit accelerometer data
	short raw_accelX = static_cast<short>((data_input->accel_x[1] << 8) | data_input->accel_x[0]);
	short raw_accelY = static_cast<short>((data_input->accel_y[1] << 8) | data_input->accel_y[0]);
	short raw_accelZ = static_cast<short>((data_input->accel_z[1] << 8) | data_input->accel_z[0]);

	// Piece together the 16-bit gyroscope data
	short raw_gyroX = static_cast<short>((data_input->gyro_x[1] << 8) | data_input->gyro_x[0]);
	short raw_gyroY = static_cast<short>((data_input->gyro_y[1] << 8) | data_input->gyro_y[0]);
	short raw_gyroZ = static_cast<short>((data_input->gyro_z[1] << 8) | data_input->gyro_z[0]);

	// Save the raw accelerometer values
	RawAccel.i = static_cast<int>(raw_accelX);
	RawAccel.j = static_cast<int>(raw_accelY);
	RawAccel.k = static_cast<int>(raw_accelZ);

	// Save the raw gyro values
	RawGyro.i = static_cast<int>(raw_gyroX);
	RawGyro.j = static_cast<int>(raw_gyroY);
	RawGyro.k = static_cast<int>(raw_gyroZ);

	// calibrated_acc= raw_acc*acc_gain
	CalibratedAccel.i = static_cast<float>(raw_accelX) * config->accelerometer_gain;
	CalibratedAccel.j = static_cast<float>(raw_accelY) * config->accelerometer_gain;
	CalibratedAccel.k = static_cast<float>(raw_accelZ) * config->accelerometer_gain;

	// calibrated_gyro= raw_gyro*gyro_gain
	CalibratedGyro.i = static_cast<float>(raw_gyroX) * config->gyro_gain;
	CalibratedGyro.j = static_cast<float>(raw_gyroY) * config->gyro_gain;
	CalibratedGyro.k = static_cast<float>(raw_gyroZ) * config->gyro_gain;
}

// -- Morpheus HMD State -----
void MorpheusHMDState::parse_data_input(
	const MorpheusHMDConfig *config, 
	const struct MorpheusDataInput *data_input)
{
	SensorFrames[0].parse_data_input(config, &data_input->imu_frame_0);
	SensorFrames[1].parse_data_input(config, &data_input->imu_frame_1);
}

// -- Morpheus HMD -----
MorpheusHMD::MorpheusHMD()
    : cfg()
    , HIDDetails(nullptr)
    , NextPollSequenceNumber(0)
    , InData(nullptr)
    , HMDStates()
{
    HIDDetails = new MorpheusHIDDetails;
    InData = new MorpheusDataInput;

    HMDStates.clear();
}

MorpheusHMD::~MorpheusHMD()
{
    if (getIsOpen())
    {
        SERVER_LOG_ERROR("~MorpheusHMD") << "HMD deleted without calling close() first!";
    }

    delete InData;
    delete HIDDetails;
}

bool MorpheusHMD::open()
{
    HMDDeviceEnumerator enumerator(CommonDeviceState::Morpheus);
    bool success = false;

    if (enumerator.is_valid())
    {
        success = open(&enumerator);
    }

    return success;
}

bool MorpheusHMD::open(
    const DeviceEnumerator *enumerator)
{
    const HMDDeviceEnumerator *pEnum = static_cast<const HMDDeviceEnumerator *>(enumerator);

    const char *cur_dev_path = pEnum->get_path();
    bool success = false;

    if (getIsOpen())
    {
        SERVER_LOG_WARNING("MorpheusHMD::open") << "MorpheusHMD(" << cur_dev_path << ") already open. Ignoring request.";
        success = true;
    }
    else
    {
		SERVER_LOG_INFO("MorpheusHMD::open") << "Opening MorpheusHMD(" << cur_dev_path << ").";

		HIDDetails->device_identifier = cur_dev_path;

		HIDDetails->sensor_device_path = pEnum->get_interface_path(MORPHEUS_SENSOR_INTERFACE);
		HIDDetails->sensor_device_handle = hid_open_path(HIDDetails->sensor_device_path.c_str());
		if (HIDDetails->sensor_device_handle != nullptr)
		{
			hid_set_nonblocking(HIDDetails->sensor_device_handle, 1);
		}

		HIDDetails->command_device_path = pEnum->get_interface_path(MORPHEUS_COMMAND_INTERFACE);
		HIDDetails->command_device_handle = hid_open_path(HIDDetails->command_device_path.c_str());
		if (HIDDetails->command_device_handle != nullptr)
		{
			hid_set_nonblocking(HIDDetails->command_device_handle, 1);
		}

        if (getIsOpen())  // Controller was opened and has an index
        {
			// Always save the config back out in case some defaults changed
			cfg.save();

            // Reset the polling sequence counter
            NextPollSequenceNumber = 0;

			success = true;
        }
        else
        {
            SERVER_LOG_ERROR("MorpheusHMD::open") << "Failed to open MorpheusHMD(" << cur_dev_path << ")";
			close();
        }
    }

    return success;
}

void MorpheusHMD::close()
{
    if (HIDDetails->sensor_device_handle != nullptr || HIDDetails->command_device_handle != nullptr)
    {
		if (HIDDetails->sensor_device_handle != nullptr)
		{
			SERVER_LOG_INFO("MorpheusHMD::close") << "Closing MorpheusHMD(" << HIDDetails->sensor_device_path << ")";
			hid_close(HIDDetails->sensor_device_handle);
		}

		if (HIDDetails->command_device_handle != nullptr)
		{
			SERVER_LOG_INFO("MorpheusHMD::close") << "Closing MorpheusHMD(" << HIDDetails->command_device_path << ")";
			hid_close(HIDDetails->command_device_handle);
		}

        HIDDetails->Reset();
        InData->Reset();
    }
    else
    {
        SERVER_LOG_INFO("MorpheusHMD::close") << "MorpheusHMD already closed. Ignoring request.";
    }
}

// Getters
bool
MorpheusHMD::matchesDeviceEnumerator(const DeviceEnumerator *enumerator) const
{
    // Down-cast the enumerator so we can use the correct get_path.
    const HMDDeviceEnumerator *pEnum = static_cast<const HMDDeviceEnumerator *>(enumerator);

    bool matches = false;

    if (pEnum->get_device_type() == getDeviceType())
    {
        const char *enumerator_path = pEnum->get_path();
        const char *dev_path = HIDDetails->device_identifier.c_str();

#ifdef _WIN32
        matches = _stricmp(dev_path, enumerator_path) == 0;
#else
        matches = strcmp(dev_path, enumerator_path) == 0;
#endif
    }

    return matches;
}

bool
MorpheusHMD::getIsReadyToPoll() const
{
    return (getIsOpen());
}

std::string
MorpheusHMD::getUSBDevicePath() const
{
    return HIDDetails->sensor_device_path;
}

bool
MorpheusHMD::getIsOpen() const
{
    return HIDDetails->sensor_device_handle != nullptr && HIDDetails->command_device_handle != nullptr;
}

IControllerInterface::ePollResult
MorpheusHMD::poll()
{
	IHMDInterface::ePollResult result = IHMDInterface::_PollResultFailure;

	if (getIsOpen())
	{
		static const int k_max_iterations = 32;

		for (int iteration = 0; iteration < k_max_iterations; ++iteration)
		{
			// Attempt to read the next update packet from the controller
			int res = hid_read(HIDDetails->sensor_device_handle, (unsigned char*)InData, sizeof(MorpheusDataInput));

			if (res == 0)
			{
				// Device still in valid state
				result = (iteration == 0)
					? IHMDInterface::_PollResultSuccessNoData
					: IHMDInterface::_PollResultSuccessNewData;

				// No more data available. Stop iterating.
				break;
			}
			else if (res < 0)
			{
				char hidapi_err_mbs[256];
				bool valid_error_mesg = 
					ServerUtility::convert_wcs_to_mbs(hid_error(HIDDetails->sensor_device_handle), hidapi_err_mbs, sizeof(hidapi_err_mbs));

				// Device no longer in valid state.
				if (valid_error_mesg)
				{
					SERVER_LOG_ERROR("PSMoveController::readDataIn") << "HID ERROR: " << hidapi_err_mbs;
				}
				result = IHMDInterface::_PollResultFailure;

				// No more data available. Stop iterating.
				break;
			}
			else
			{
				// New data available. Keep iterating.
				result = IHMDInterface::_PollResultSuccessNewData;
			}

			// https://github.com/hrl7/node-psvr/blob/master/lib/psvr.js
			MorpheusHMDState newState;

			// Increment the sequence for every new polling packet
			newState.PollSequenceNumber = NextPollSequenceNumber;
			++NextPollSequenceNumber;

			// Processes the IMU data
			newState.parse_data_input(&cfg, InData);

			// Make room for new entry if at the max queue size
			if (HMDStates.size() >= MORPHEUS_HMD_STATE_BUFFER_MAX)
			{
				HMDStates.erase(HMDStates.begin(), HMDStates.begin() + HMDStates.size() - MORPHEUS_HMD_STATE_BUFFER_MAX);
			}

			HMDStates.push_back(newState);
		}
	}

	return result;
}

void
MorpheusHMD::getTrackingShape(CommonDeviceTrackingShape &outTrackingShape) const
{
	outTrackingShape.shape_type = eCommonTrackingShapeType::PointCloud;
	//###HipsterSloth $TODO - Fill in points
	outTrackingShape.shape.point_cloud.point[0].set(0.f, 0.f, 0.f);
	outTrackingShape.shape.point_cloud.point[1].set(0.f, 0.f, 0.f);
	outTrackingShape.shape.point_cloud.point[2].set(0.f, 0.f, 0.f);
	outTrackingShape.shape.point_cloud.point[3].set(0.f, 0.f, 0.f);
	outTrackingShape.shape.point_cloud.point[4].set(0.f, 0.f, 0.f);
	outTrackingShape.shape.point_cloud.point[5].set(0.f, 0.f, 0.f);
	outTrackingShape.shape.point_cloud.point[6].set(0.f, 0.f, 0.f);
}

bool 
MorpheusHMD::getTrackingColorID(eCommonTrackingColorID &out_tracking_color_id) const
{
	out_tracking_color_id = eCommonTrackingColorID::Blue;
	return true;
}

const CommonDeviceState *
MorpheusHMD::getState(
    int lookBack) const
{
    const int queueSize = static_cast<int>(HMDStates.size());
    const CommonDeviceState * result =
        (lookBack < queueSize) ? &HMDStates.at(queueSize - lookBack - 1) : nullptr;

    return result;
}

long MorpheusHMD::getMaxPollFailureCount() const
{
    return cfg.max_poll_failure_count;
}