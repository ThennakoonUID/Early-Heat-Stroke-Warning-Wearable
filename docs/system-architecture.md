# System Architecture

## Overview

The device is a compact wearable patch, worn on the skin, that continuously monitors two physiological signals correlated with impending heat stroke: **sweat cessation (anhidrosis)** and **skin temperature**.

```
 SKIN CONTACT
 ┌────────────────────────────┐
 │ Ag/AgCl Electrodes (GSR)   │──┐
 │ NTC Thermistor (Temp)      │──┤
 └────────────────────────────┘  │
                                  ▼
                        ┌───────────────────┐
                        │  Low-power MCU     │
                        │  - ADC sampling     │
                        │  - Sensor fusion     │
                        │  - Anhidrosis logic  │
                        └───────────────────┘
                                  │ BLE
                                  ▼
                        ┌───────────────────┐
                        │  Mobile App /      │
                        │  Site Dashboard     │
                        │  - Alert display     │
                        │  - Trend logging     │
                        └───────────────────┘
```

## Sensing

- **Galvanic Skin Response (GSR):** Ag/AgCl electrode pair measures sweat conductivity. A sustained drop in conductivity while temperature is rising is the anhidrosis signature.
- **Skin Temperature:** NTC thermistor tracks localized skin temperature trend, used both as an input to the fusion algorithm and a secondary alert trigger.
- **Planned addition:** heart rate / respiratory rate (PPG or ECG-lead based), recommended by the consulting Medical Officer to reduce false positives from single-indicator triggers.

## Processing

A low-energy microcontroller (e.g., nRF52 / ESP32-class) samples both channels, runs a lightweight on-device fusion algorithm (rule-based or simple ML), and classifies state as: **Normal → Risk Rising → Anhidrosis Detected / Alert**.

## Output

Alerts are transmitted over Bluetooth Low Energy to a companion mobile app or a site-supervisor dashboard, enabling intervention before collapse — replacing subjective visual observation ("buddy system") with objective, continuous data.

## Design Constraints

- Must be low-cost and ruggedized enough for construction/agricultural/military field use.
- Battery life must support a full field shift.
- Must avoid the invasiveness/cost of ingestible core-temperature pills.
- Should ideally add HR/respiratory rate to reduce single-indicator false alarms (clinician recommendation).
