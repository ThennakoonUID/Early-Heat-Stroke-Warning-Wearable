# Early-Heat-Stroke-Warning-Wearable
A low-cost wearable patch that continuously monitors anhidrosis (sweat cessation) and skin temperature to give an actionable early warning of heat stroke — before physiological collapse occurs.

Built by **Team VitaSync**, BM2210 Biomedical Device Design, Dept. of Electronic & Telecommunication Engineering, University of Moratuwa.

---

## 🚨 The Problem

Heat stroke is a severe medical emergency caused by thermoregulatory failure. Current field practice relies on **subjective, symptom-based observation** (sweating status, patient-reported discomfort) rather than continuous data. By the time visible symptoms appear, the person may already be close to collapse.

Consulting a Medical Officer (National Teaching Hospital, Kandy) confirmed the key gaps:

- **Single-indicator risk** - anhidrosis alone isn't a reliable standalone trigger; heart rate and other vitals matter too.
- **Symptom-based, not data-based** - assessments depend on the worker/patient reporting symptoms.
- **Field monitoring gap** - human error, no continuous monitoring, and high equipment cost limit current site-safety practices.

High-risk populations: outdoor laborers (construction, agriculture), military personnel in hot climates, athletes, and the elderly during heatwaves.

## 💡 The Solution

A compact wearable patch that fuses two physiological signals in real time:

| Sensor | Signal | Purpose |
|---|---|---|
| Ag/AgCl electrodes | Galvanic Skin Response (sweat conductivity) | Detect onset of anhidrosis |
| NTC thermistor | Skin temperature | Track rising thermal load |
| (Planned) HR/PPG sensor | Heart rate | Cross-validate risk (clinician-recommended) |

A low-energy microcontroller runs on-device sensor-fusion to flag the anhidrosis + temperature pattern that precedes heat stroke, and raises an alert before collapse - instead of relying on visual observation or a "buddy system."

**Need statement:**
> "A way to continuously monitor the onset of anhidrosis (sweat cessation) in individuals exposed to high environmental heat or prolonged physical exertion, in order to provide an actionable early warning of heat stroke before systemic physiological collapse occurs."

## 🎯 Why This Need (of 5 investigated)

The team evaluated five biomedical device opportunities (heat stroke detection, physiotherapy ROM tracking, CKDu screening, diabetic foot ulcer monitoring, dental caries detection via fluorescence) against Estimated Market, Patient Impact, Provider Impact, Feasibility, and Team Preference. **Heat stroke early detection scored highest (20/25)** - see [`docs/need-selection.md`](docs/need-selection.md) and the full [investigation report](docs/BDD_Investigation_Report.pdf).

## 🧩 System Architecture

```
[Skin Contact]
   ├─ Ag/AgCl Electrodes ──► GSR sensing ─┐
   └─ NTC Thermistor ──────► Temp sensing ┼─► MCU (sensor fusion) ──► BLE ──► Mobile App / Alert
                                           │
                          (future: PPG/HR)┘
```

## 🗺️ Roadmap

- [x] Need finding & market validation (5 opportunities investigated)
- [x] Opportunity selected: heat stroke early warning wearable
- [ ] Sensor selection & bench-top GSR/temperature validation
- [ ] Circuit design & first prototype patch
- [ ] Firmware: sensor-fusion alert algorithm
- [ ] Companion mobile app (BLE alerts)
- [ ] Field / bench testing against anhidrosis-onset benchmarks
- [ ] Add heart rate / respiratory rate channel (per clinician recommendation)

## 👥 Team VitaSync

| Name | Index No. |
|---|---|
| Thennakoon U.I.D. | 240657J |
| Pitiduwa S.M. | 240518K |
| Tennekone A.D.T.M.S.H. | 240642J |
| Wanninayaka W.M.I.R. | 240695X |
| Thiyamini M.W.T. | 240669X |

Course: **BM2210 - Biomedical Device Design**, Dept. of Electronic and Telecommunication Engineering, University of Moratuwa.
