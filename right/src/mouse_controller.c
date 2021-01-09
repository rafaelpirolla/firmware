#include <math.h>
#include "key_action.h"
#include "led_display.h"
#include "layer.h"
#include "usb_interfaces/usb_interface_mouse.h"
#include "peripherals/test_led.h"
#include "slave_drivers/is31fl3xxx_driver.h"
#include "slave_drivers/uhk_module_driver.h"
#include "timer.h"
#include "config_parser/parse_keymap.h"
#include "usb_commands/usb_command_get_debug_buffer.h"
#include "arduino_hid/ConsumerAPI.h"
#include "secondary_role_driver.h"
#include "slave_drivers/touchpad_driver.h"
#include "mouse_controller.h"
#include "slave_scheduler.h"
#include "layer_switcher.h"
#include "usb_report_updater.h"

static uint32_t mouseUsbReportUpdateTime = 0;
static uint32_t mouseElapsedTime;

uint8_t ActiveMouseStates[ACTIVE_MOUSE_STATES_COUNT];
uint8_t ToggledMouseStates[ACTIVE_MOUSE_STATES_COUNT];

bool CompensateDiagonalSpeed = false;

static float expDriver(float x, float y);
static void recalculateCurrentSpeed(float inx, float iny);

mouse_kinetic_state_t MouseMoveState = {
    .isScroll = false,
    .upState = SerializedMouseAction_MoveUp,
    .downState = SerializedMouseAction_MoveDown,
    .leftState = SerializedMouseAction_MoveLeft,
    .rightState = SerializedMouseAction_MoveRight,
    .verticalStateSign = 0,
    .horizontalStateSign = 0,
    .intMultiplier = 25,
    .initialSpeed = 5,
    .acceleration = 35,
    .deceleratedSpeed = 10,
    .baseSpeed = 40,
    .acceleratedSpeed = 80,
};

mouse_kinetic_state_t MouseScrollState = {
    .isScroll = true,
    .upState = SerializedMouseAction_ScrollDown,
    .downState = SerializedMouseAction_ScrollUp,
    .leftState = SerializedMouseAction_ScrollLeft,
    .rightState = SerializedMouseAction_ScrollRight,
    .verticalStateSign = 0,
    .horizontalStateSign = 0,
    .intMultiplier = 1,
    .initialSpeed = 20,
    .acceleration = 20,
    .deceleratedSpeed = 10,
    .baseSpeed = 20,
    .acceleratedSpeed = 50,
};

static void updateOneDirectionSign(int8_t* sign, int8_t expectedSign, uint8_t expectedState, uint8_t otherState) {
    if (*sign == expectedSign && !ActiveMouseStates[expectedState]) {
        *sign = ActiveMouseStates[otherState] ? -expectedSign : 0;
    }
}

// Assume that mouse movement key has been just released. In that case check if there is another key which keeps the state active.
// If not, check whether the other direction state is active and either flip movement direction or zero the state.
static void updateDirectionSigns(mouse_kinetic_state_t *kineticState) {
    updateOneDirectionSign(&kineticState->horizontalStateSign, -1, kineticState->leftState, kineticState->rightState);
    updateOneDirectionSign(&kineticState->horizontalStateSign,  1, kineticState->rightState, kineticState->leftState);
    updateOneDirectionSign(&kineticState->verticalStateSign, -1, kineticState->upState, kineticState->downState);
    updateOneDirectionSign(&kineticState->verticalStateSign,  1, kineticState->downState, kineticState->upState);
}

// Called on keydown of mouse action. Direction signs ensure that the last pressed action always takes precedence, and therefore
// have to be updated statefully.
void MouseController_ActivateDirectionSigns(uint8_t state) {
    switch (state) {
    case SerializedMouseAction_MoveUp:
        MouseMoveState.verticalStateSign = -1;
        break;
    case SerializedMouseAction_MoveDown:
        MouseMoveState.verticalStateSign = 1;
        break;
    case SerializedMouseAction_MoveLeft:
        MouseMoveState.horizontalStateSign = -1;
        break;
    case SerializedMouseAction_MoveRight:
        MouseMoveState.horizontalStateSign = 1;
        break;
    case SerializedMouseAction_ScrollUp:
        MouseScrollState.verticalStateSign = 1;
        break;
    case SerializedMouseAction_ScrollDown:
        MouseScrollState.verticalStateSign = -1;
        break;
    case SerializedMouseAction_ScrollLeft:
        MouseScrollState.horizontalStateSign = -1;
        break;
    case SerializedMouseAction_ScrollRight:
        MouseScrollState.horizontalStateSign = 1;
        break;
    }
}

static void processMouseKineticState(mouse_kinetic_state_t *kineticState)
{
    float initialSpeed = kineticState->intMultiplier * kineticState->initialSpeed;
    float acceleration = kineticState->intMultiplier * kineticState->acceleration;
    float deceleratedSpeed = kineticState->intMultiplier * kineticState->deceleratedSpeed;
    float baseSpeed = kineticState->intMultiplier * kineticState->baseSpeed;
    float acceleratedSpeed = kineticState->intMultiplier * kineticState->acceleratedSpeed;

    if (!kineticState->wasMoveAction && !ActiveMouseStates[SerializedMouseAction_Decelerate]) {
        kineticState->currentSpeed = initialSpeed;
    }

    bool doublePressedStateExists = ActiveMouseStates[kineticState->upState] > 1 ||
            ActiveMouseStates[kineticState->downState] > 1 ||
            ActiveMouseStates[kineticState->leftState] > 1 ||
            ActiveMouseStates[kineticState->rightState] > 1;

    bool isMoveAction = ActiveMouseStates[kineticState->upState] ||
                        ActiveMouseStates[kineticState->downState] ||
                        ActiveMouseStates[kineticState->leftState] ||
                        ActiveMouseStates[kineticState->rightState];

    mouse_speed_t mouseSpeed = MouseSpeed_Normal;
    if (ActiveMouseStates[SerializedMouseAction_Accelerate] || doublePressedStateExists) {
        kineticState->targetSpeed = acceleratedSpeed;
        mouseSpeed = MouseSpeed_Accelerated;
    } else if (ActiveMouseStates[SerializedMouseAction_Decelerate]) {
        kineticState->targetSpeed = deceleratedSpeed;
        mouseSpeed = MouseSpeed_Decelerated;
    } else if (isMoveAction) {
        kineticState->targetSpeed = baseSpeed;
    }

    if (mouseSpeed == MouseSpeed_Accelerated || (kineticState->wasMoveAction && isMoveAction && (kineticState->prevMouseSpeed != mouseSpeed))) {
        kineticState->currentSpeed = kineticState->targetSpeed;
    }

    if (isMoveAction) {
        if (kineticState->currentSpeed < kineticState->targetSpeed) {
            kineticState->currentSpeed += acceleration * (float)mouseElapsedTime / 1000.0f;
            if (kineticState->currentSpeed > kineticState->targetSpeed) {
                kineticState->currentSpeed = kineticState->targetSpeed;
            }
        } else {
            kineticState->currentSpeed -= acceleration * (float)mouseElapsedTime / 1000.0f;
            if (kineticState->currentSpeed < kineticState->targetSpeed) {
                kineticState->currentSpeed = kineticState->targetSpeed;
            }
        }

        float distance = kineticState->currentSpeed * (float)mouseElapsedTime / 1000.0f;


        if (kineticState->isScroll && !kineticState->wasMoveAction) {
            kineticState->xSum = 0;
            kineticState->ySum = 0;
        }

        // Update travelled distances

        updateDirectionSigns(kineticState);

        if ( kineticState->horizontalStateSign != 0 && kineticState->verticalStateSign != 0 && CompensateDiagonalSpeed ) {
            distance /= 1.41f;
        }

        kineticState->xSum += distance * kineticState->horizontalStateSign;
        kineticState->ySum += distance * kineticState->verticalStateSign;

        // Update horizontal state

        bool horizontalMovement = kineticState->horizontalStateSign != 0;

        float xSumInt;
        float xSumFrac = modff(kineticState->xSum, &xSumInt);
        kineticState->xSum = xSumFrac;
        kineticState->xOut = xSumInt;

        // Handle the first scroll tick.
        if (kineticState->isScroll && !kineticState->wasMoveAction && kineticState->xOut == 0 && horizontalMovement) {
            kineticState->xOut = ActiveMouseStates[kineticState->leftState] ? -1 : 1;
            kineticState->xSum = 0;
        }

        // Update vertical state

        bool verticalMovement = kineticState->verticalStateSign != 0;

        float ySumInt;
        float ySumFrac = modff(kineticState->ySum, &ySumInt);
        kineticState->ySum = ySumFrac;
        kineticState->yOut = ySumInt;

        // Handle the first scroll tick.
        if (kineticState->isScroll && !kineticState->wasMoveAction && kineticState->yOut == 0 && verticalMovement) {
            kineticState->yOut = ActiveMouseStates[kineticState->upState] ? -1 : 1;
            kineticState->ySum = 0;
        }
    } else {
        kineticState->currentSpeed = 0;
    }

    kineticState->prevMouseSpeed = mouseSpeed;
    kineticState->wasMoveAction = isMoveAction;
}

uint8_t touchpadScrollDivisor = 8;

static void processTouchpadActions(float* outX, float*outY) {
    recalculateCurrentSpeed(TouchpadEvents.x, TouchpadEvents.y);
    float q = expDriver(TouchpadEvents.x, TouchpadEvents.y);
    *outX += q*TouchpadEvents.x;
    *outY += q*TouchpadEvents.y;
    TouchpadEvents.x = 0;
    TouchpadEvents.y = 0;

    uint8_t wheelXInteger = TouchpadEvents.wheelX / touchpadScrollDivisor;
    if (wheelXInteger) {
        ActiveUsbMouseReport->wheelX += wheelXInteger;
        TouchpadEvents.wheelX = TouchpadEvents.wheelX % touchpadScrollDivisor;
    }

    uint8_t wheelYInteger = TouchpadEvents.wheelY / touchpadScrollDivisor;
    if (wheelYInteger) {
        ActiveUsbMouseReport->wheelY -= wheelYInteger;
        TouchpadEvents.wheelY = TouchpadEvents.wheelY % touchpadScrollDivisor;
    }


    if (TouchpadEvents.singleTap) {
        ActiveUsbMouseReport->buttons |= MouseButton_Left;
        TouchpadEvents.singleTap = false;
    }

    if (TouchpadEvents.twoFingerTap) {
        ActiveUsbMouseReport->buttons |= MouseButton_Right;
        TouchpadEvents.twoFingerTap = false;
    }

    if (TouchpadEvents.tapAndHold) {
        ActiveUsbMouseReport->buttons |= MouseButton_Left;
    }
}

//This is current calculated pointer speed in px/s
static float currentSpeed = 0.0f;

static void recalculateCurrentSpeed(float x, float y) {
    if(x != 0 || y != 0) {
        static uint16_t lastUpdate = 0;
        currentSpeed = (float)sqrt(x*x + y*y) * 1000.0f / (CurrentTime - lastUpdate);
        lastUpdate = CurrentTime;
    }
}

/*
 * Interpretation
 * - (baseSpeedCoef) is fixed part of the multiplier, which is not affected by acceleration
 * - (midSpeedCoef)  is variable part of the multiplier, which is then affected by acceleration exponent
 * - (baseSpeedCoef + midSpeedCoef) is speed multiplier achieved at speed midSpeed (px/s).
 *
 * For simple configuration, leave baseSpeedCoef and midSpeed unchanged, while exposing:
 * - speed        = midSpeedCoef
 * - acceleration = accelerationExp
 */
static float baseSpeedCoef = 0.0f;
static float midSpeed = 3000.0f;
static float midSpeedCoef = 1.0f;
static float accelerationExp = 0.5f;

static float expDriver(float x, float y)
{
    float normalizedSpeed = currentSpeed/midSpeed;
    return baseSpeedCoef + midSpeedCoef*(float)pow(normalizedSpeed, accelerationExp);
}

void MouseController_SetExpDriverParams(float baseSpeedCoef_, float midSpeedCoef_, float accelerationExp_, float midSpeed_)
{
    baseSpeedCoef = baseSpeedCoef_;
    midSpeed = midSpeed_;
    midSpeedCoef = midSpeedCoef_;
    accelerationExp = accelerationExp_;

}

#define NONE 0
#define LX 1
#define LY 2

void MouseController_ProcessMouseActions()
{
    const uint8_t wC = 10;
    const uint8_t wTrsh = 2;
    static uint8_t wLastX = NONE;
    static int8_t wX = 0;
    static int8_t wY = 0;
    static uint32_t lastUpdate = 0;
    static uint32_t lastTick = 0;

    static float sumX = 0.0f;
    static float sumY = 0.0f;
    static bool firstTick = false;
    bool moveDeltaChanged = false;

    mouseElapsedTime = Timer_GetElapsedTimeAndSetCurrent(&mouseUsbReportUpdateTime);

    processMouseKineticState(&MouseMoveState);
    ActiveUsbMouseReport->x = MouseMoveState.xOut;
    ActiveUsbMouseReport->y = MouseMoveState.yOut;
    MouseMoveState.xOut = 0;
    MouseMoveState.yOut = 0;

    processMouseKineticState(&MouseScrollState);
    ActiveUsbMouseReport->wheelX = MouseScrollState.xOut;
    ActiveUsbMouseReport->wheelY = MouseScrollState.yOut;
    MouseScrollState.xOut = 0;
    MouseScrollState.yOut = 0;

    if (Slaves[SlaveId_RightTouchpad].isConnected) {
        moveDeltaChanged = true;
        processTouchpadActions(&sumX, &sumY);
    }

    for (uint8_t moduleId=0; moduleId<UHK_MODULE_MAX_COUNT; moduleId++) {
        uhk_module_state_t *moduleState = UhkModuleStates + moduleId;
        if (moduleState->pointerCount) {
            moveDeltaChanged = true;
            switch(moduleState -> moduleId) {
            case ModuleId_KeyClusterLeft:
                ActiveUsbMouseReport->wheelX += moduleState->pointerDelta.x;
                ActiveUsbMouseReport->wheelY -= moduleState->pointerDelta.y;
//                if(moduleState -> pointerDelta.x != 0 || moduleState -> pointerDelta.y != 0) {
//                    if(CurrentTime - lastUpdate > 1000) {
//                        wX = 0;
//                        wY = 0;
//                        wLastX = NONE;
//                    }
//                    lastUpdate = CurrentTime;
//                }
//
//                wX += moduleState->pointerDelta.x;
//                wY += moduleState->pointerDelta.y;
//
//                if((wX >= wC && wLastX == LX) || wX >= wC*wTrsh) {
//                    ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_RIGHT_ARROW;
//                    wX = 0;
//                    wY = 0;
//                    wLastX = LX;
//                }
//                if((wX <= -wC && wLastX == LX) || wX <= -wC*wTrsh) {
//                    ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_LEFT_ARROW;
//                    wX = 0;
//                    wY = 0;
//                    wLastX = LX;
//                }
//                if((wY >= wC && wLastX == LY) || wY >= wC*wTrsh) {
//                    ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_DOWN_ARROW;
//                    wX = 0;
//                    wY = 0;
//                    wLastX = LY;
//                }
//                if((wY <= -wC && wLastX == LY) || wY <= -wC*wTrsh) {
//                    ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_UP_ARROW;
//                    wX = 0;
//                    wY = 0;
//                    wLastX = LY;
//                }
                break;
            case ModuleId_TouchpadRight:
            {
                /** Nothing is here, look elsewhere! */
            }
                break;
            case ModuleId_TrackballRight:
            case ModuleId_TrackpointRight:
            {
                float x = (int16_t)moduleState->pointerDelta.x;
                float y = (int16_t)moduleState->pointerDelta.y;
                recalculateCurrentSpeed(x, y);
                float q = expDriver(x, y);
                sumX += q*x;
                sumY -= q*y;
            }
            }
            moduleState->pointerDelta.x = 0;
            moduleState->pointerDelta.y = 0;
        }
    }

    bool scrollMode = ActiveLayer == LayerId_Mouse;
    bool caretMode = ActiveLayer == LayerId_Fn;
    float scrollSpeedDivisor = 8.0f;
    if(moveDeltaChanged) {
        float xSumInt;
        float ySumInt;
        if(scrollMode) {
            sumX /= scrollSpeedDivisor;
            sumY /= scrollSpeedDivisor;
        }
        sumX = modff(sumX, &xSumInt);
        sumY = modff(sumY, &ySumInt);
        if(scrollMode) {
            ActiveUsbMouseReport->wheelX += xSumInt;
            ActiveUsbMouseReport->wheelY -= ySumInt;
            sumX *= scrollSpeedDivisor;
            sumY *= scrollSpeedDivisor;
        } else if (caretMode) {
            if(xSumInt != 0 || ySumInt != 0) {
                if(firstTick && CurrentTime - lastUpdate < 100 && false) {
                    xSumInt = 0;
                    ySumInt = 0;
                } else {
                    firstTick = false;

                    if(CurrentTime - lastUpdate > 500) {
                        wX = 0;
                        wY = 0;
                        wLastX = NONE;
                    }

                    lastUpdate = CurrentTime;
                }
            }

            wX += xSumInt;
            wY += ySumInt;

#define FIRST_TICK(ax) (ax > wC && wLastX == NONE)
#define FIRST_TICKB(ax) (ax > wC && wLastX == NONE)

            uint8_t prevMode = wLastX;

            if((wX >= wC && wLastX == LX) || wX >= wC*wTrsh || FIRST_TICK(wX)) {
                ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_RIGHT_ARROW;
                wX = 0;
                wY = 0;
                wLastX = LX;
                lastTick = CurrentTime;
            }
            if((wX <= -wC && wLastX == LX) || wX <= -wC*wTrsh || FIRST_TICK(-wX)) {
                ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_LEFT_ARROW;
                wX = 0;
                wY = 0;
                wLastX = LX;
                lastTick = CurrentTime;
            }
            if((wY >= wC && wLastX == LY) || wY >= wC*wTrsh || FIRST_TICK(wY)) {
                ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_DOWN_ARROW;
                wX = 0;
                wY = 0;
                wLastX = LY;
                lastTick = CurrentTime;
            }
            if((wY <= -wC && wLastX == LY) || wY <= -wC*wTrsh || FIRST_TICK(-wY)) {
                ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = HID_KEYBOARD_SC_UP_ARROW;
                wX = 0;
                wY = 0;
                wLastX = LY;
                lastTick = CurrentTime;
            }

            if(wLastX != NONE && prevMode == NONE) {
                firstTick = true;
            }
        } else {
            ActiveUsbMouseReport->x += xSumInt;
            ActiveUsbMouseReport->y += ySumInt;
        }
    }

//  The following line makes the firmware crash for some reason:
//  SetDebugBufferFloat(60, mouseScrollState.currentSpeed);
//  TODO: Figure out why.
//  Oddly, the following line (which is the inlined version of the above) works:
//  *(float*)(DebugBuffer + 60) = mouseScrollState.currentSpeed;
//  The value parameter of SetDebugBufferFloat() seems to be the culprit because
//  if it's not used within the function it doesn't crash anymore.

    if (ActiveMouseStates[SerializedMouseAction_LeftClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Left;
    }
    if (ActiveMouseStates[SerializedMouseAction_MiddleClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Middle;
    }
    if (ActiveMouseStates[SerializedMouseAction_RightClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Right;
    }
    if (ActiveMouseStates[SerializedMouseAction_Button_4]) {
        ActiveUsbMouseReport->buttons |= MouseButton_4;
    }
    if (ActiveMouseStates[SerializedMouseAction_Button_5]) {
        ActiveUsbMouseReport->buttons |= MouseButton_5;
    }
    if (ActiveMouseStates[SerializedMouseAction_Button_6]) {
        ActiveUsbMouseReport->buttons |= MouseButton_6;
    }
    if (ActiveMouseStates[SerializedMouseAction_Button_7]) {
        ActiveUsbMouseReport->buttons |= MouseButton_7;
    }
    if (ActiveMouseStates[SerializedMouseAction_Button_8]) {
        ActiveUsbMouseReport->buttons |= MouseButton_8;
    }
}

void ToggleMouseState(serialized_mouse_action_t action, bool activate)
{
    if (activate) {
        ToggledMouseStates[action]++;
        // First macro action is ran during key update cycle, i.e., after ActiveMouseStates is copied from ToggledMouseStates.
        // Otherwise, direction sign will be resetted at the end of this cycle
        ActiveMouseStates[action]++;
        MouseController_ActivateDirectionSigns(action);
    }
    else{
        ToggledMouseStates[action] -= ToggledMouseStates[action] > 0 ? 1 : 0;
    }
}
