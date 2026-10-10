import 'package:flutter/material.dart';
import 'package:firebase_core/firebase_core.dart';
import 'firebase_options.dart';
import 'package:firebase_database/firebase_database.dart';
import 'dart:ui';
import 'dart:math' as math;
import 'dart:async';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:permission_handler/permission_handler.dart';

final FlutterLocalNotificationsPlugin flutterLocalNotificationsPlugin =
    FlutterLocalNotificationsPlugin();

Future<void> requestNotificationPermission() async {
  if (await Permission.notification.isDenied) {
    await Permission.notification.request();
  }
}

void initializeNotifications() async {
  const AndroidInitializationSettings initializationSettingsAndroid =
      AndroidInitializationSettings('@mipmap/ic_launcher');

  const InitializationSettings initializationSettings =
      InitializationSettings(android: initializationSettingsAndroid);

  await flutterLocalNotificationsPlugin.initialize(initializationSettings);
}

// 1. Medium Warning සඳහා දුරකථනයේ Default Notification Sound එක යැවීම
Future<void> showMediumWarningNotification() async {
  const AndroidNotificationDetails androidPlatformChannelSpecifics =
      AndroidNotificationDetails(
    'medium_risk_channel',
    'Medium Risk Warnings',
    channelDescription: 'Notifications for medium health risks',
    importance: Importance.high,
    priority: Priority.high,
    // මෙහි දුරකථනයේ සාමාන්‍ය නොටිෆිකේෂන් සවුන්ඩ් එක ස්වයංක්‍රීයව භාවිත වේ
  );

  const NotificationDetails platformChannelSpecifics =
      NotificationDetails(android: androidPlatformChannelSpecifics);

  await flutterLocalNotificationsPlugin.show(
    0,
    'Medium Risk Warning',
    'Your vital signs show a medium risk level. Please be careful.',
    platformChannelSpecifics,
  );
}

// 2. Critical Warning සඳහා දුරකථනයේ Loud Alarm / Ringtone ශබ්දය යැවීම
Future<void> showCriticalWarningNotification() async {
  const AndroidNotificationDetails androidPlatformChannelSpecifics =
      AndroidNotificationDetails(
    'critical_risk_channel',
    'Critical Risk Warnings',
    channelDescription: 'Notifications for severe health risks',
    importance: Importance.max,
    priority: Priority.max,
    // දුරකථනයේ Inbuilt Alarm ශබ්දය ලබා දීමට sound එක රින්ග්ටෝන් ලෙස සකසයි
    playSound: true,
  );

  const NotificationDetails platformChannelSpecifics =
      NotificationDetails(android: androidPlatformChannelSpecifics);

  await flutterLocalNotificationsPlugin.show(
    1,
    'CRITICAL WARNING!',
    'Severe Heat Stroke Risk Detected! Immediate medical attention required.',
    platformChannelSpecifics,
  );
}

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  
  initializeNotifications();
  await requestNotificationPermission();
  
  await Firebase.initializeApp(
    options: DefaultFirebaseOptions.currentPlatform,
  );
  
  FirebaseDatabase.instance.databaseURL = 'https://heatstrockapp-default-rtdb.asia-southeast1.firebasedatabase.app';
  
  runApp(const HeatStrokeApp());
}

class HeatStrokeApp extends StatelessWidget {
  const HeatStrokeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Heat Stroke Monitor',
      theme: ThemeData(
        brightness: Brightness.light,
        scaffoldBackgroundColor: const Color(0xFFF4F8FB),
        primaryColor: const Color(0xFF0077B6),
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF0077B6),
        ),
        appBarTheme: const AppBarTheme(
          backgroundColor: Color(0xFF0077B6),
          foregroundColor: Colors.white,
          elevation: 0,
        ),
      ),
      home: const IntroScreen(),
    );
  }
}

// Intro Screen
class IntroScreen extends StatefulWidget {
  const IntroScreen({super.key});

  @override
  State<IntroScreen> createState() => _IntroScreenState();
}

class _IntroScreenState extends State<IntroScreen> {
  @override
  void initState() {
    super.initState();
    Timer(const Duration(milliseconds: 3500), () {
      Navigator.pushReplacement(
        context,
        MaterialPageRoute(builder: (context) => const SerialInputDialog()),
      );
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.white,
      body: Center(
        child: Image.asset(
          'assets/intro.gif',
          width: 280,
          height: 280,
          fit: BoxFit.contain,
        ),
      ),
    );
  }
}

// Serial Input Dialog with VitaSync Background Image
class SerialInputDialog extends StatefulWidget {
  const SerialInputDialog({super.key});

  @override
  _SerialInputDialogState createState() => _SerialInputDialogState();
}

class _SerialInputDialogState extends State<SerialInputDialog> {
  final TextEditingController _controller = TextEditingController();

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Container(
        decoration: const BoxDecoration(
          image: DecorationImage(
            image: AssetImage('assets/vitasync_bg.png'),
            fit: BoxFit.cover,
          ),
        ),
        child: Align(
          alignment: Alignment.bottomCenter,
          child: Padding(
            padding: const EdgeInsets.only(bottom: 40.0, left: 24.0, right: 24.0),
            child: Container(
              padding: const EdgeInsets.all(24),
              decoration: BoxDecoration(
                color: Colors.white.withOpacity(0.95),
                borderRadius: BorderRadius.circular(20),
                boxShadow: [
                  BoxShadow(
                    color: Colors.blue.withOpacity(0.15),
                    blurRadius: 20,
                    spreadRadius: 5,
                  ),
                ],
              ),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  const Text(
                    "Enter Device Serial Number",
                    style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold, color: Color(0xFF03045E)),
                  ),
                  const SizedBox(height: 20),
                  TextField(
                    controller: _controller,
                    decoration: InputDecoration(
                      border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
                      labelText: 'Serial Number (e.g., DEV-001)',
                      filled: true,
                      fillColor: const Color(0xFFF8FAFC),
                    ),
                  ),
                  const SizedBox(height: 20),
                  SizedBox(
                    width: double.infinity,
                    height: 50,
                    child: ElevatedButton(
                      style: ElevatedButton.styleFrom(
                        backgroundColor: const Color(0xFF0077B6),
                        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                      ),
                      onPressed: () {
                        String serialNo = _controller.text.trim();
                        if (serialNo.isNotEmpty) {
                          Navigator.push(
                            context,
                            MaterialPageRoute(
                              builder: (context) => DashboardScreen(serialNumber: serialNo),
                            ),
                          );
                        }
                      },
                      child: const Text("Connect Device", style: TextStyle(fontSize: 16, color: Colors.white, fontWeight: FontWeight.bold)),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class DashboardScreen extends StatefulWidget {
  final String serialNumber;
  const DashboardScreen({super.key, required this.serialNumber});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  bool _hasPlayedMediumWarning = false;
  bool _alarmMutedByUser = false;

  void _stopAlarm() {
    setState(() {
      _alarmMutedByUser = true;
    });
  }

  @override
  Widget build(BuildContext context) {
    DatabaseReference dbRef = FirebaseDatabase.instance.ref().child('devices/${widget.serialNumber}');

    return Scaffold(
      appBar: AppBar(
        title: Text("Monitor: ${widget.serialNumber}"),
        backgroundColor: const Color(0xFF0077B6),
      ),
      body: StreamBuilder(
        stream: dbRef.onValue,
        builder: (context, snapshot) {
          if (snapshot.connectionState == ConnectionState.waiting) {
            return const Center(child: CircularProgressIndicator());
          }
          
          if (!snapshot.hasData || snapshot.data!.snapshot.value == null) {
            return const Center(child: Text("No Data Found for this Serial Number!", style: TextStyle(color: Colors.black54)));
          }

          var data = snapshot.data!.snapshot.value as Map<dynamic, dynamic>;
          double skinTemp = double.tryParse(data['skin_temp'].toString()) ?? 0.0;
          double outsideTemp = double.tryParse(data['outside_temp'].toString()) ?? 0.0;
          double feelsLikeTemp = double.tryParse(data['feels_like_temp'].toString()) ?? (outsideTemp + 2.0);
          int heartRate = int.tryParse(data['heart_rate'].toString()) ?? 72;
          
          String sweatValStr = (data['sweat_level'] ?? '5').toString();
          int sweatLevelScore = int.tryParse(sweatValStr) ?? (sweatValStr.toLowerCase() == 'high' ? 8 : (sweatValStr.toLowerCase() == 'medium' ? 5 : 2));
          sweatLevelScore = sweatLevelScore.clamp(1, 10);

          bool isHighRisk = skinTemp > 38.0 || heartRate > 110;
          bool isMediumRisk = (skinTemp > 37.0 && skinTemp <= 38.0) || (heartRate > 90 && heartRate <= 110);
          bool isSweatRisk = skinTemp >= 38.0 && sweatLevelScore < 4;

          // Notification / System Sound Logic
          if (isHighRisk) {
            if (!_alarmMutedByUser) {
              showCriticalWarningNotification();
            }
            _hasPlayedMediumWarning = false;
          } else {
            _alarmMutedByUser = false;
            if (isMediumRisk || isSweatRisk) {
              if (!_hasPlayedMediumWarning) {
                showMediumWarningNotification();
                _hasPlayedMediumWarning = true; // එක් වරක් පමණක් නාද වීමට
              }
            } else {
              _hasPlayedMediumWarning = false; // සාමාන්‍ය තත්ත්වයට පත්වූ පසු රීසට් වේ
            }
          }

          Color hrColor;
          String hrStatusText;
          String hrWarningMsg;
          if (heartRate > 110) {
            hrColor = Colors.redAccent;
            hrStatusText = "HIGH RISK";
            hrWarningMsg = "Your heart rate is high. You need immediate medical treatments.";
          } else if (heartRate > 90) {
            hrColor = Colors.amber.shade700;
            hrStatusText = "MEDIUM RISK";
            hrWarningMsg = "Your heart rate is little high. Maybe there will be a risk.";
          } else {
            hrColor = Colors.green;
            hrStatusText = "NORMAL";
            hrWarningMsg = "Heart rate is normal and stable.";
          }

          Color tempColor;
          String tempStatusText;
          String tempWarningMsg;
          if (skinTemp > 38.0) {
            tempColor = Colors.redAccent;
            tempStatusText = "HIGH RISK";
            tempWarningMsg = "Body temperature is high. You need immediate medical treatments.";
          } else if (skinTemp > 37.0) {
            tempColor = Colors.amber.shade700;
            tempStatusText = "MEDIUM RISK";
            tempWarningMsg = "Body temperature is little high. Maybe there will be a risk.";
          } else {
            tempColor = const Color(0xFFD97706);
            tempStatusText = "NORMAL";
            tempWarningMsg = "Body temperature is safe.";
          }

          Color outColor;
          String outWarningMsg;
          if (outsideTemp > 38.0) {
            outColor = Colors.redAccent;
            outWarningMsg = "Outside temp is very high! Be careful when working outside.";
          } else if (outsideTemp > 33.0) {
            outColor = Colors.amber.shade700;
            outWarningMsg = "Outside temp is little high. Be careful when working outside.";
          } else {
            outColor = const Color(0xFF0284C7);
            outWarningMsg = "Outside weather is normal.";
          }

          return Stack(
            children: [
              Padding(
                padding: const EdgeInsets.all(16.0),
                child: ListView(
                  children: [
                    const SizedBox(height: 10),
                    TempCard(
                      title: "Body Temperature", 
                      value: skinTemp, 
                      unit: "°C", 
                      minVal: 30.0,
                      maxVal: 45.0,
                      color: tempColor, 
                      cardColor: Colors.white,
                      warningMessage: tempWarningMsg,
                      statusText: tempStatusText,
                    ),
                    const SizedBox(height: 14),

                    // Weather & Environment Card (Split into Half)
                    Container(
                      padding: const EdgeInsets.all(16),
                      decoration: BoxDecoration(
                        color: Colors.white,
                        borderRadius: BorderRadius.circular(20),
                        border: Border.all(color: outColor.withOpacity(0.4), width: 1.5),
                        boxShadow: [
                          BoxShadow(
                            color: outColor.withOpacity(0.08),
                            blurRadius: 12,
                            spreadRadius: 2,
                          ),
                        ],
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              const Text("Weather & Environment", style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: Color(0xFF03045E))),
                              Container(
                                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                                decoration: BoxDecoration(
                                  color: outColor.withOpacity(0.15),
                                  borderRadius: BorderRadius.circular(10),
                                ),
                                child: Text(
                                  outsideTemp > 33 ? "CAUTION" : "NORMAL",
                                  style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: outColor),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 12),
                          Row(
                            children: [
                              Expanded(
                                child: Column(
                                  crossAxisAlignment: CrossAxisAlignment.start,
                                  children: [
                                    const Text("Outside Temp", style: TextStyle(fontSize: 12, color: Colors.grey)),
                                    const SizedBox(height: 4),
                                    Text("${outsideTemp.toStringAsFixed(1)} °C", style: TextStyle(fontSize: 22, fontWeight: FontWeight.bold, color: outColor)),
                                  ],
                                ),
                              ),
                              Container(height: 35, width: 1, color: Colors.grey[300]),
                              const SizedBox(width: 12),
                              Expanded(
                                child: Column(
                                  crossAxisAlignment: CrossAxisAlignment.start,
                                  children: [
                                    const Text("Feels Like", style: TextStyle(fontSize: 12, color: Colors.grey)),
                                    const SizedBox(height: 4),
                                    Text("${feelsLikeTemp.toStringAsFixed(1)} °C", style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold, color: Color(0xFFD97706))),
                                  ],
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 8),
                          Container(
                            padding: const EdgeInsets.all(8),
                            decoration: BoxDecoration(
                              color: outColor.withOpacity(0.08),
                              borderRadius: BorderRadius.circular(8),
                            ),
                            child: Text(
                              outWarningMsg,
                              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: outColor),
                            ),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 14),

                    // Heart Rate Card
                    Container(
                      padding: const EdgeInsets.all(16),
                      decoration: BoxDecoration(
                        color: Colors.white,
                        borderRadius: BorderRadius.circular(20),
                        border: Border.all(color: hrColor.withOpacity(0.4), width: 2),
                        boxShadow: [
                          BoxShadow(
                            color: hrColor.withOpacity(0.08),
                            blurRadius: 15,
                            spreadRadius: 3,
                          ),
                        ],
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              const Text(
                                "Heart Rate",
                                style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: Color(0xFF03045E)),
                              ),
                              Container(
                                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                                decoration: BoxDecoration(
                                  color: hrColor.withOpacity(0.15),
                                  borderRadius: BorderRadius.circular(10),
                                ),
                                child: Text(
                                  hrStatusText,
                                  style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: hrColor),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 12),
                          Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              TweenAnimationBuilder<double>(
                                tween: Tween<double>(begin: heartRate.toDouble(), end: heartRate.toDouble()),
                                duration: const Duration(milliseconds: 600),
                                builder: (context, animatedValue, child) {
                                  return Text(
                                    "${animatedValue.toInt()} BPM",
                                    style: TextStyle(fontSize: 26, fontWeight: FontWeight.bold, color: hrColor),
                                  );
                                },
                              ),
                              Icon(Icons.favorite, color: hrColor, size: 32),
                            ],
                          ),
                          const SizedBox(height: 8),
                          Container(
                            padding: const EdgeInsets.all(8),
                            decoration: BoxDecoration(
                              color: hrColor.withOpacity(0.08),
                              borderRadius: BorderRadius.circular(8),
                            ),
                            child: Text(
                              hrWarningMsg,
                              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: hrColor),
                            ),
                          ),
                          const SizedBox(height: 12),
                          ClipRRect(
                            borderRadius: BorderRadius.circular(10),
                            child: Container(
                              height: 16,
                              color: Colors.grey[100],
                              child: Stack(
                                children: [
                                  AnimatedFractionallySizedBox(
                                    duration: const Duration(milliseconds: 600),
                                    curve: Curves.easeInOut,
                                    widthFactor: (heartRate / 150.0).clamp(0.0, 1.0),
                                    child: Container(
                                      decoration: BoxDecoration(
                                        gradient: LinearGradient(
                                          colors: [
                                            hrColor.withOpacity(0.6),
                                            hrColor,
                                          ],
                                        ),
                                        borderRadius: BorderRadius.circular(10),
                                      ),
                                      child: const WaterWaveEffect(),
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 14),

                    // Sweat Level Meter Card
                    Container(
                      padding: const EdgeInsets.all(16),
                      decoration: BoxDecoration(
                        color: Colors.white,
                        borderRadius: BorderRadius.circular(20),
                        border: Border.all(
                          color: isSweatRisk ? Colors.redAccent.withOpacity(0.6) : const Color(0xFFE2E8F0), 
                          width: isSweatRisk ? 2 : 1.5,
                        ),
                        boxShadow: [
                          BoxShadow(
                            color: isSweatRisk ? Colors.redAccent.withOpacity(0.15) : Colors.blue.withOpacity(0.05),
                            blurRadius: 12,
                            spreadRadius: 2,
                          ),
                        ],
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              const Text(
                                "Sweat Level Meter",
                                style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: Color(0xFF03045E)),
                              ),
                              Container(
                                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                                decoration: BoxDecoration(
                                  color: isSweatRisk ? Colors.red.withOpacity(0.15) : Colors.blue.withOpacity(0.1),
                                  borderRadius: BorderRadius.circular(10),
                                ),
                                child: Text(
                                  isSweatRisk ? "HEAT STROKE RISK!" : "Level: $sweatLevelScore / 10",
                                  style: TextStyle(
                                    fontSize: 12, 
                                    fontWeight: FontWeight.bold, 
                                    color: isSweatRisk ? Colors.red : const Color(0xFF0077B6),
                                  ),
                                ),
                              ),
                            ],
                          ),
                          const SizedBox(height: 8),
                          if (isSweatRisk)
                            Container(
                              margin: const EdgeInsets.only(bottom: 8),
                              padding: const EdgeInsets.all(6),
                              decoration: BoxDecoration(
                                color: Colors.red[50],
                                borderRadius: BorderRadius.circular(6),
                              ),
                              child: const Text(
                                "Warning: Body temperature is high but sweat level is critically low!",
                                style: TextStyle(fontSize: 11, fontWeight: FontWeight.bold, color: Colors.red),
                              ),
                            ),
                          const SizedBox(height: 4),
                          Row(
                            children: List.generate(10, (index) {
                              bool isActive = index < sweatLevelScore;
                              Color barColor;
                              if (isSweatRisk) {
                                barColor = Colors.redAccent;
                              } else {
                                if (index < 4) {
                                  barColor = Colors.green;
                                } else if (index < 7) {
                                  barColor = Colors.amber;
                                } else {
                                  barColor = Colors.redAccent;
                                }
                              }

                              return Expanded(
                                child: Container(
                                  height: 18,
                                  margin: const EdgeInsets.symmetric(horizontal: 2.0),
                                  decoration: BoxDecoration(
                                    color: isActive ? barColor : Colors.grey[200],
                                    borderRadius: BorderRadius.circular(4),
                                    boxShadow: isActive
                                        ? [
                                            BoxShadow(
                                              color: barColor.withOpacity(0.4),
                                              blurRadius: 4,
                                              spreadRadius: 1,
                                            )
                                          ]
                                        : [],
                                  ),
                                ),
                              );
                            }),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),

              // High Risk Warning Popup with Alarm Off Button
              if (isHighRisk && !_alarmMutedByUser)
                TweenAnimationBuilder<double>(
                  tween: Tween<double>(begin: 0.0, end: 1.0),
                  duration: const Duration(milliseconds: 500),
                  builder: (context, opacity, child) {
                    return Opacity(
                      opacity: opacity,
                      child: BackdropFilter(
                        filter: ImageFilter.blur(sigmaX: 5.0, sigmaY: 5.0),
                        child: Container(
                          color: Colors.black.withOpacity(0.3),
                          child: Center(
                            child: Transform.scale(
                              scale: 0.8 + (0.2 * opacity),
                              child: Container(
                                margin: const EdgeInsets.symmetric(horizontal: 24),
                                padding: const EdgeInsets.all(24),
                                decoration: BoxDecoration(
                                  color: Colors.white,
                                  borderRadius: BorderRadius.circular(24),
                                  border: Border.all(color: Colors.redAccent, width: 2),
                                  boxShadow: [
                                    BoxShadow(
                                      color: Colors.redAccent.withOpacity(0.3),
                                      blurRadius: 25,
                                      spreadRadius: 5,
                                    ),
                                  ],
                                ),
                                child: Column(
                                  mainAxisSize: MainAxisSize.min,
                                  children: [
                                    const Icon(Icons.warning_amber_rounded, color: Colors.redAccent, size: 60),
                                    const SizedBox(height: 16),
                                    const Text(
                                      "CRITICAL WARNING!",
                                      style: TextStyle(
                                        fontSize: 22, 
                                        fontWeight: FontWeight.bold, 
                                        color: Colors.redAccent,
                                        letterSpacing: 1.2,
                                      ),
                                    ),
                                    const SizedBox(height: 8),
                                    const Text(
                                      "Severe Heat Stroke Risk Detected!\nImmediate medical attention is required.",
                                      textAlign: TextAlign.center,
                                      style: TextStyle(fontSize: 15, color: Color(0xFF334155), height: 1.4),
                                    ),
                                    const SizedBox(height: 20),
                                    Container(
                                      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                                      decoration: BoxDecoration(
                                        color: Colors.red[50],
                                        borderRadius: BorderRadius.circular(12),
                                      ),
                                      child: Text(
                                        "Body Temp: $skinTemp °C | HR: $heartRate BPM",
                                        style: const TextStyle(fontSize: 14, fontWeight: FontWeight.bold, color: Colors.red),
                                      ),
                                    ),
                                    const SizedBox(height: 24),
                                    SizedBox(
                                      width: double.infinity,
                                      height: 48,
                                      child: ElevatedButton(
                                        style: ElevatedButton.styleFrom(
                                          backgroundColor: Colors.redAccent,
                                          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                                        ),
                                        onPressed: _stopAlarm,
                                        child: const Text(
                                          "ALARM OFF",
                                          style: TextStyle(fontSize: 16, color: Colors.white, fontWeight: FontWeight.bold),
                                        ),
                                      ),
                                    ),
                                  ],
                                ),
                              ),
                            ),
                          ),
                        ),
                      ),
                    );
                  },
                ),
            ],
          );
        },
      ),
    );
  }
}

class WaterWaveEffect extends StatefulWidget {
  const WaterWaveEffect({super.key});

  @override
  _WaterWaveEffectState createState() => _WaterWaveEffectState();
}

class _WaterWaveEffectState extends State<WaterWaveEffect> with SingleTickerProviderStateMixin {
  late AnimationController _controller;

  @override
  void initState() {
    super.initState();
    _controller = AnimationController(
      vsync: this,
      duration: const Duration(seconds: 2),
    )..repeat();
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _controller,
      builder: (context, child) {
        return CustomPaint(
          painter: WavePainter(_controller.value),
          child: const SizedBox.expand(),
        );
      },
    );
  }
}

class WavePainter extends CustomPainter {
  final double animationValue;
  WavePainter(this.animationValue);

  @override
  void paint(Canvas canvas, Size size) {
    Paint paint = Paint()
      ..color = Colors.white.withOpacity(0.35)
      ..style = PaintingStyle.fill;

    Path path = Path();
    path.moveTo(0, size.height);
    
    for (double i = 0; i <= size.width; i++) {
      double dx = i;
      double dy = size.height * 0.5 +
          math.sin((animationValue * 360 + i) * math.pi / 180 * 2) * 3;
      path.lineTo(dx, dy);
    }

    path.lineTo(size.width, size.height);
    path.close();
    canvas.drawPath(path, paint);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => true;
}

class TempCard extends StatelessWidget {
  final String title;
  final double value;
  final String unit;
  final double minVal;
  final double maxVal;
  final Color color;
  final Color cardColor;
  final String warningMessage;
  final String statusText;

  const TempCard({
    super.key, 
    required this.title, 
    required this.value, 
    required this.unit, 
    required this.minVal,
    required this.maxVal,
    required this.color,
    required this.cardColor,
    required this.warningMessage,
    required this.statusText,
  });

  @override
  Widget build(BuildContext context) {
    double percentage = ((value - minVal) / (maxVal - minVal)).clamp(0.0, 1.0);

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: cardColor,
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: color.withOpacity(0.4), width: 1.5),
        boxShadow: [
          BoxShadow(
            color: color.withOpacity(0.08),
            blurRadius: 12,
            spreadRadius: 2,
          ),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(title, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: Color(0xFF03045E))),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                decoration: BoxDecoration(
                  color: color.withOpacity(0.15),
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Text(
                  statusText,
                  style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: color),
                ),
              ),
            ],
          ),
          const SizedBox(height: 8),
          
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              TweenAnimationBuilder<double>(
                tween: Tween<double>(begin: value, end: value),
                duration: const Duration(milliseconds: 600),
                builder: (context, animatedValue, child) {
                  return Text(
                    "${animatedValue.toStringAsFixed(1)} $unit",
                    style: TextStyle(fontSize: 26, fontWeight: FontWeight.bold, color: color),
                  );
                },
              ),
            ],
          ),
          const SizedBox(height: 8),

          Container(
            padding: const EdgeInsets.all(8),
            decoration: BoxDecoration(
              color: color.withOpacity(0.08),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Text(
              warningMessage,
              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: color),
            ),
          ),
          const SizedBox(height: 12),

          Column(
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Text("${minVal.toInt()}°", style: TextStyle(fontSize: 10, color: Colors.grey[500])),
                  Text("${maxVal.toInt()}°", style: TextStyle(fontSize: 10, color: Colors.grey[500])),
                ],
              ),
              const SizedBox(height: 4),
              
              LayoutBuilder(
                builder: (context, constraints) {
                  double totalWidth = constraints.maxWidth;
                  return Stack(
                    alignment: Alignment.centerLeft,
                    children: [
                      Container(
                        height: 8,
                        width: totalWidth,
                        decoration: BoxDecoration(
                          color: Colors.grey[200],
                          borderRadius: BorderRadius.circular(4),
                        ),
                      ),
                      AnimatedContainer(
                        duration: const Duration(milliseconds: 600),
                        curve: Curves.easeInOut,
                        height: 8,
                        width: totalWidth * percentage,
                        decoration: BoxDecoration(
                          color: color,
                          borderRadius: BorderRadius.circular(4),
                        ),
                      ),
                      AnimatedPositioned(
                        duration: const Duration(milliseconds: 600),
                        curve: Curves.easeInOut,
                        left: (totalWidth - 12) * percentage,
                        child: Container(
                          width: 12,
                          height: 12,
                          decoration: BoxDecoration(
                            color: Colors.white,
                            shape: BoxShape.circle,
                            border: Border.all(color: color, width: 2.5),
                            boxShadow: [
                              BoxShadow(
                                color: color.withOpacity(0.4),
                                blurRadius: 4,
                                spreadRadius: 1,
                              ),
                            ],
                          ),
                        ),
                      ),
                    ],
                  );
                },
              ),
            ],
          ),
        ],
      ),
    );
  }
}