# VitaSync: Smart Real-Time Heat Stroke Monitoring & Early Warning System 🌡️📱

> An advanced IoT and mobile-integrated health monitoring application designed to detect early signs of heat stress and prevent heat stroke in real-time. Developed using **Flutter** and **Firebase**.

---

## 🚀 About the Project

Environmental heat stress is a growing health concern, particularly in tropical climates and outdoor labor-intensive environments. **VitaSync** bridges the gap between wearable health hardware and smart mobile alerts. It connects directly with IoT sensor nodes (via Firebase Realtime Database) to continuously track critical vital signs—including skin temperature, heart rate, and sweat levels—and triggers immediate multi-tier visual and auditory safety warnings before critical conditions occur.

---

## ✨ Key Features

*   **Real-Time Vitals Tracking:** Live dashboard updating core health metrics streamed from IoT devices:
    *   **Body (Skin) Temperature & Outside Environment:** Dynamic gauge progress bars with intuitive risk-level color coding.
    *   **Heart Rate Monitor:** Animated live BPM counter featuring a smooth water-wave effect and status indicators.
    *   **Sweat Level Meter:** 10-tier interactive meter tracking dehydration and sweat activity to catch dangerous heat stroke precursors early.
*   **Intelligent Risk Assessment & Anomaly Detection:** Utilizes threshold logic combined with data anomaly detection (Isolation Forest) to flag abnormal physiological shifts.
*   **Multi-Tier Alert System:**
    *   *Medium Risk:* Triggers single system-level warning notifications.
    *   *Critical Risk:* Activates continuous priority alerts, immersive warning dialogs with device metrics, and quick-access emergency "Alarm Off" controls.
*   **Sleek & Modern UI/UX:**
    *   Animated GIF introduction screen.
    *   Branded authentication and device pairing interface via Serial Number input (`DEV-001`, etc.).
    *   Responsive layouts optimized for Android and iOS.

---

## 🛠️ Tech Stack & Architecture

*   **Frontend Framework:** Flutter (Dart)
*   **Backend & Database:** Firebase Realtime Database & Firebase Core
*   **Notifications & Permissions:** `flutter_local_notifications`, `permission_handler`
*   **Machine Learning (Anomaly Detection):** Python (`scikit-learn`, `Isolation Forest`, `joblib`)
*   **State Management & Streams:** Native Flutter `StreamBuilder` with real-time Firebase listeners
*   **Build & Compilation:** Gradle (Kotlin DSL) with Core Library Desugaring enabled for broad Android version compatibility

---

## 📱 App UI Structure

1. **Introductory Splash:** Smooth animated loading sequence (`intro.gif`).
2. **Device Pairing Screen:** Clean secure background login where users input their hardware device serial number.
3. **Live Monitoring Dashboard:** Real-time cards displaying Body Temperature, Environmental Conditions, Heart Rate, and Sweat Level status indicators.
4. **Critical Warning Modal:** High-contrast blur-filtered popup that locks attention and plays high-priority system alerts during medical emergencies.

---

## ⚙️ Getting Started & Installation

### Prerequisites
*   [Flutter SDK](https://docs.flutter.dev/get-started/install) (^3.13.0 or higher)
*   Python 3.x (for running the ML anomaly detection bridge)
*   Android Studio / VS Code with Flutter & Dart extensions
*   An active Firebase project with Realtime Database configured

### Step 1: Clone the Repository
```bash
git clone [https://github.com/your-username/heat-stroke-monitor.git](https://github.com/your-username/heat-stroke-monitor.git)
cd heat-stroke-monitor
```

### Step 2: Install Flutter Dependencies
```bash
flutter pub get
```

### Step 3: Configure Firebase
1. Set up your Firebase project and add your Android/iOS apps.
2. Place your generated `google-services.json` inside `android/app/`.
3. Ensure your Firebase Realtime Database URL is correctly specified in `main.dart`:
   ```dart
   FirebaseDatabase.instance.databaseURL = '[https://your-database-region.firebasedatabase.app](https://your-database-region.firebasedatabase.app)';
   ```

### Step 4: Run the Python ML Anomaly Detection Script
1. Place your Firebase `serviceAccountKey.json` in your backend Python directory.
2. Install required Python packages:
   ```bash
   pip install firebase-admin scikit-learn pandas joblib numpy
   ```
3. Run the detection script to bridge real-time analysis:
   ```bash
   python app.py
   ```

### Step 5: Build & Run the App
To run the app on a connected device or emulator:
```bash
flutter run
```

To build a release APK:
```bash
flutter clean
flutter pub get
flutter build apk --release
```

---

## 🗄️ Firebase Database Schema Reference

The app listens to node structures formatted as follows:
```json
{
  "devices": {
    "DEV-001": {
      "skin_temp": 37.2,
      "outside_temp": 32.5,
      "feels_like_temp": 34.5,
      "heart_rate": 78,
      "sweat_level": 5,
      "anomaly_detected": false
    }
  }
}
```

---

## 💡 Future Enhancements
*   Integration of an on-device TensorFlow Lite (TFLite) machine learning model for localized edge anomaly detection.
*   GPS location tracking to factor in ambient humidity and direct sunlight index.
*   Automated SMS emergency dispatch for critical risk alerts.

---

## 👨‍💻 Author
Developed as part of biomedical engineering hardware-software integration research focused on wearable continuous health monitoring.
