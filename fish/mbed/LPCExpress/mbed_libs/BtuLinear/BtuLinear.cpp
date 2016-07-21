#include "BtuLinear.h"

BtuLinear::BtuLinear():
    m_depthPid(DEP_K_C, DEP_TAU_I, DEP_TAU_D, PID_FREQ, DEPTH_MIN, DEPTH_MAX, VEL_MIN, VEL_MAX, 0),
    m_posAPid(SP_K_C, SP_TAU_I, SP_TAU_D, PID_FREQ, POS_MIN, POS_MAX, VEL_MIN, VEL_MAX, 0),
    m_posBPid(SP_K_C, SP_TAU_I, SP_TAU_D, PID_FREQ, POS_MIN, POS_MAX, VEL_MIN, VEL_MAX, 0),
    m_velAPid(VEL_K_C, VEL_TAU_I, VEL_TAU_D, PID_FREQ, VEL_MIN, VEL_MAX, -1, 1, 0),
    m_velBPid(VEL_K_C, VEL_TAU_I, VEL_TAU_D, PID_FREQ, VEL_MIN, VEL_MAX, -1, 1, 0),
	m_pressureSensor(PIN_IMU_SDA, PIN_IMU_SCL),
    m_actAPwm(PIN_ACTA_PWM),
    m_actBPwm(PIN_ACTB_PWM),
    m_actAPot(PIN_ACTA_POT),
    m_actBPot(PIN_ACTB_POT),
    m_actADir(PIN_ACTA_DIR),
    m_actBDir(PIN_ACTB_DIR)
{};

BtuLinear::~BtuLinear(){}

void BtuLinear::init() {
    m_mode = DEFAULT_CTRL_MODE;

    // default gain values for depth controller
    m_kc = DEP_K_C;
    m_tauI = DEP_TAU_I;
    m_tauD = DEP_TAU_D;

    // default gain values for position controller
    m_p_kc = SP_K_C;
    m_p_tauI = SP_TAU_I;
    m_p_tauD = SP_TAU_D;

    // default gain values for velocity controller
    m_v_kc = VEL_K_C;
    m_v_tauI = VEL_TAU_I;
    m_v_tauD = VEL_TAU_D;

    // initialize Pressure Sensor
    m_pressureSensor.MS5837Init();
    m_pressureSensor.MS5837Start();
    wait(0.1);                    // remnant from old BTU class TODO: check if can be removed

    // initialize old position readings for Actuators
    m_oldPosA = getActPosition(ACT_A);
    m_oldPosB = getActPosition(ACT_B);

    // initialize the Windows for SMA to 0
    for(int i = 0; i < AVG_WINDOW_WIDTH; i++) {
        m_avg_windowA[i] = 0;
        m_avg_windowB[i] = 0;
    }

    // initialize all pointers, size of window to reflect that it is empty
    m_avg_windowPtr = 0;
    m_avg_windowSize = 0;
    m_currentAvgA = 0;

    m_currentAvgB = 0;

    // initialize starting voltage for velocity control to 0
    m_currentVoltage = 0;
}

// return a pressure reading
float BtuLinear::getPressure() {
    return m_pressureSensor.MS5837_Pressure();
}

// resets values of the controllers
void BtuLinear::stop() {
	m_currentVoltage = 0;
	m_currentAvgA = 0;
	m_currentAvgB = 0;
	m_avg_windowSize = 0;
	m_depthPid.reset();
	m_posAPid.reset();
	m_posBPid.reset();
	m_velAPid.reset();
	m_velBPid.reset();
	return;
}

// updates depth PID tunings
void BtuLinear::updateDepthTunings(float kc, float tauI, float tauD) {
    m_kc = kc;
    m_tauI = tauI;
    m_tauD = tauD;
    m_depthPid.setTunings(kc, tauI, tauD);
}

// updates Position PID tunings
void BtuLinear::updatePosTunings(float kc, float tauI, float tauD) {
    m_p_kc = kc;
    m_p_tauI = tauI;
    m_p_tauD = tauD;
    m_posAPid.setTunings(kc, tauI, tauD);
    m_posBPid.setTunings(kc, tauI, tauD);
}

// updates Velocity PID tunings
void BtuLinear::updateVelTunings(float kc, float tauI, float tauD) {
    m_v_kc = kc;
    m_v_tauI = tauI;
    m_v_tauD = tauD;
    m_velAPid.setTunings(kc, tauI, tauD);
    m_velBPid.setTunings(kc, tauI, tauD);
}

// updates Mode.  Resets most values if the mode has changed
void BtuLinear::updateMode(int mode) {
    if(m_mode != mode) {
        stop();
        m_mode = mode;
    }
}

// runs one cycle of the controller dictated by mode
void BtuLinear::runCycle(float setVal) {
    switch (m_mode) {

    case VOLTAGE_CTRL_MODE:
        voltageControl(setVal);
        break;

    case VELOCITY_CTRL_MODE:
        velocityControl(setVal);
        break;

    case DEPTH_CTRL_MODE:
        depthControl(setVal);
        break;

    case POSITION_CTRL_MODE:
        positionControl(setVal);
        break;
    }
}

// convenience function, updates mode, then runs a cycle in the chosen mode
void BtuLinear::updateAndRunCycle(int mode, float value) {
    updateMode(mode);
    runCycle(value);
}

// sends a duty cycle to one of the actuators, based on the second argument
void BtuLinear::voltageControlHelper(float setDuty, int ctrl) {
    // PwmOut* actPwm;
    // DigitalOut* actDir;
    // if(ctrl == ACT_A) {
    //     actPwm = &m_actAPwm;
    //     actDir = &m_actADir;
    // } else {
    //     actPwm = &m_actBPwm;
    //     actDir = &m_actBDir;
    // }
    // if(setDuty > 0) {
    //     *actPwm = setDuty;
    //     *actDir = 1;
    // } else {
    //     *actDir = 0;
    //     *actPwm = -setDuty;
    // }
    if(ctrl == ACT_A) {
        if(setDuty > 0) {       // extending
            m_actAPwm = setDuty;
            m_actADir = 1;
        } else {                // contracting
            m_actADir = 0;
            m_actAPwm = -setDuty;
        }
    } else {
        if(setDuty > 0) {
            m_actBPwm = setDuty;
            m_actBDir = 1;
        } else {
            m_actBDir = 0;
            m_actBPwm = -setDuty;
        }
    }
}

// calls voltageControlHelper on both actuators
void BtuLinear::voltageControl(float setDuty) {
    voltageControlHelper(setDuty, ACT_A);
    voltageControlHelper(setDuty, ACT_B);
}

// updates the SMA window with the current position reading
void BtuLinear::updatePositionReadings() {
    float aPosition = m_actAPot;
    float bPosition = m_actBPot;

    float aOldPos = m_avg_windowA[m_avg_windowPtr];
    float bOldPos = m_avg_windowB[m_avg_windowPtr];
    m_avg_windowA[m_avg_windowPtr] = aPosition;
    m_avg_windowB[m_avg_windowPtr] = bPosition;
    m_avg_windowPtr = (m_avg_windowPtr+1) % AVG_WINDOW_WIDTH;
    if(m_avg_windowSize >= AVG_WINDOW_WIDTH) {
        // buffer is full
    	m_currentAvgA = m_currentAvgA + (aPosition / AVG_WINDOW_WIDTH)- (aOldPos / AVG_WINDOW_WIDTH);
        m_currentAvgB = m_currentAvgB + (bPosition / AVG_WINDOW_WIDTH)- (bOldPos / AVG_WINDOW_WIDTH);
    } else {
    	// buffer is still filling up
        m_avg_windowSize++;
        m_currentAvgA = 0;
        m_currentAvgB = 0;
        for(int i = 0; i < m_avg_windowSize; i++) {
            m_currentAvgA = (m_avg_windowA[i] / m_avg_windowSize);
            m_currentAvgB = (m_avg_windowB[i] / m_avg_windowSize);
        }
    }
}

// gets the current Actuator Position.  No SMA, just reads and rescales the potentiometer
float BtuLinear::getActPosition(int act) {
    // updatePositionReadings();
    // float position;
    // if(act == ACT_A) {
    //     position = m_currentAvgA;
    // } else {
    //     position = m_currentAvgB;
    // }
    float position;
    if(act == ACT_A) {
        position = m_actAPot;
    } else {
        position = m_actBPot;
    }
    float scaledPos = (position - POT_MIN) / (POT_MAX - POT_MIN);
    return scaledPos;
}

// controls velocity on one actuator
void BtuLinear::velocityControlHelper(float setVelocity, int ctrl) {
    // Check if one of the two allowable actuators is called
	if(ctrl != ACT_A && ctrl != ACT_B) {
        return;
    }
    // Acquire the actuator position from the potentiometer
	m_actPosVC = getActPosition(ctrl);

    // avoid going past the very edges:
	// Currently at 0.99 and 0.01 due to some uncertainty in scaling/pot readings
    // also to help avoid accumulation of error during such a period
    if ((m_actPosVC <= POSITION_MIN && setVelocity < 0) || (m_actPosVC >= POSITION_MAX && setVelocity > 0)) {
		//at an edge, set the setVelocity to 0
        m_setVelVC = 0;
    }
    else {
    	// not at an edge, use setVelocity
    	m_setVelVC = setVelocity;
    }

    if(ctrl == ACT_A) {
        m_actVelVC = (m_actPosVC - m_oldPosA) / PID_FREQ;
        // set process value to the derivative of position (velocity)
    	m_velAPid.setProcessValue(m_actVelVC);
        m_velAPid.setSetPoint(m_setVelVC);
        m_deltaVoltVC = m_velAPid.compute();
        m_oldPosA = m_actPosVC;        // update old position for so we can keep calculating derivatives
    }
    else {
    	m_actVelVC = (m_actPosVC - m_oldPosB) / PID_FREQ;
        m_velBPid.setProcessValue(m_actVelVC);
        m_velBPid.setSetPoint(m_setVelVC);
        m_deltaVoltVC = m_velBPid.compute();
        m_oldPosB = m_actPosVC;
    }
    //add the voltage delta to the current voltage that is the current operating point
    m_currentVoltage = utility::clip(m_currentVoltage + m_deltaVoltVC, -1.0, 1.0); // increase or decrease current voltage to get closer to desired velocity

    // arrest any movement at the edges, and reset accumulated voltage, to aid responsiveness at the edges
    if ((m_actPosVC <= POSITION_MIN && m_setVelVC <= 0) || (m_actPosVC >= POSITION_MAX && m_setVelVC >= 0)) {
        m_currentVoltage = 0;
    }

    m_cmdVoltVC = m_currentVoltage;

    // get absolute value for dead zone
    m_absCmdVoltVC = (m_cmdVoltVC >= 0) ? m_cmdVoltVC : -m_cmdVoltVC;

    // have a small dead zone TODO: either better tune or remove,
    // causes small offsets in position sometimes (of about 0.025% at current gain values)
    if(m_absCmdVoltVC <= VOLTAGE_THRESHOLD) {
        m_cmdVoltVC = 0;
    }
    voltageControlHelper(m_cmdVoltVC, ctrl);

}

// does velocity control on both actuators
void BtuLinear::velocityControl(float setVel) {
    velocityControlHelper(setVel, ACT_A);
    velocityControlHelper(setVel, ACT_B);
}

// control position of one actuator
void BtuLinear::positionControlHelper(float setPos, int ctrl) {
    if(ctrl == ACT_A) {
        m_posAPid.setSetPoint(setPos);
        m_posAPid.setProcessValue(getActPosition(ACT_A));
        float cmdVoltA = m_posAPid.compute();
        voltageControlHelper(cmdVoltA, ACT_A);
    } else {
        m_posBPid.setSetPoint(setPos);
        m_posBPid.setProcessValue(getActPosition(ACT_B));
        float cmdVoltB = m_posBPid.compute();
        voltageControlHelper(cmdVoltB, ACT_B);
    }

}

// control position of both actuators
void BtuLinear::positionControl(float setPos) {
    positionControlHelper(setPos, ACT_A);
    positionControlHelper(setPos, ACT_B);
}

// control depth via master-slave
void BtuLinear::depthControlHelper(float cmdVelocity) {
    velocityControlHelper(cmdVelocity, ACT_B); // control velocity on one actuator
    positionControlHelper(getActPosition(ACT_B), ACT_A); // have the other actuator constantly try to mirror the first
}

// do depth control
void BtuLinear::depthControl(float setDepthMeters) {
    m_depthPid.setSetPoint(setDepthMeters);

    float curDepth = getDepth();

    m_depthPid.setProcessValue(curDepth);

    float cmdVel = m_depthPid.compute();
    depthControlHelper(-1*cmdVel);
    // velocityControl(cmdVel);
}

// get a depth reading
float BtuLinear::getDepth() {
    float pvDepth = getPressure();
    float pvDepthMeters = (pvDepth - P_ATMOS_MBAR) / P_WATER_SURFACE_MBAR;
    return pvDepthMeters;
}

// float BTU::getServoPos() {
// 	return m_motorServo.readPosition();
// }
