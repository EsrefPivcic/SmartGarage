import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:smart_garage/src/services/firebase_service.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  _HomeScreenState createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final FirebaseService _firebaseService = FirebaseService();
  String detectedCar = "no car";
  String image = "";
  bool isAutoMode = true;
  bool manualOpen = false;

  @override
  void initState() {
    super.initState();

    _firebaseService.autoModeStream().listen((autoMode) {
      setState(() {
        isAutoMode = autoMode;
      });
    });

    _firebaseService.manualOpenStream().listen((open) {
      setState(() {
        manualOpen = open;
      });
    });

    _firebaseService.detectedCarStream().listen((car) {
      setState(() {
        detectedCar = car;
      });
    });

    _firebaseService.imageStream().listen((newImage) {
      setState(() {
        image = newImage;
      });
    });
  }

  void _toggleAutoMode(bool value) {
    _firebaseService.setAutoMode(value);
  }

  void _toggleManualOpen(bool value) {
    _firebaseService.setManualOpen(value);
  }

  void _setNewCar(String newCar) {
    _firebaseService.setNewCar(newCar);
  }

  final _newCarModelController = TextEditingController();

  void _showChangeUsernameForm() {
    final formKey = GlobalKey<FormState>();

    showDialog(
      context: context,
      builder: (BuildContext context) {
        return AlertDialog(
          title: const Text('Specify your car model'),
          content: SingleChildScrollView(
            child: Form(
              key: formKey,
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  TextFormField(
                    controller: _newCarModelController,
                    decoration:
                        const InputDecoration(labelText: 'New Car Model'),
                    validator: (value) {
                      if (value == null || value.isEmpty) {
                        return "Car model name can't be empty";
                      }
                      if (num.tryParse(value) is num) {
                        return "Car model names can't be numeric";
                      }
                      return null;
                    },
                  ),
                ],
              ),
            ),
          ),
          actions: [
            TextButton(
              onPressed: () {
                _newCarModelController.clear();
                Navigator.of(context).pop();
              },
              child: const Text('Cancel'),
            ),
            TextButton(
              onPressed: () async {
                if (formKey.currentState?.validate() ?? false) {
                  try {
                    _setNewCar(_newCarModelController.text);
                    _newCarModelController.clear();
                    Navigator.of(context).pop();
                    ScaffoldMessenger.of(context).showSnackBar(
                      const SnackBar(
                        content: Text('Car model changed successfully!'),
                      ),
                    );
                  } catch (e) {
                    Navigator.of(context).pop();
                    _newCarModelController.clear();
                    ScaffoldMessenger.of(context).showSnackBar(
                      SnackBar(
                        content: Text(e.toString()),
                      ),
                    );
                  }
                }
              },
              child: const Text('Save'),
            ),
          ],
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        backgroundColor: Color.fromARGB(255, 46, 41, 49),
        title: const Text("SmartGarage"),
        centerTitle: true,
      ),
      body: Padding(
        padding: const EdgeInsets.all(16.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              "Detected car: ${detectedCar == 'audi' ? 'Audi R8' : detectedCar == 'porsche' ? 'Porsche Cayenne S' : detectedCar == 'no car' ? 'No car present' : detectedCar}",
              style: const TextStyle(fontSize: 18, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 20),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                const Text(
                  "Auto Mode",
                  style: TextStyle(fontSize: 18),
                ),
                Switch(
                  value: isAutoMode,
                  onChanged: (value) {
                    _toggleAutoMode(value);
                  },
                ),
              ],
            ),
            if (!isAutoMode) ...[
              const SizedBox(height: 20),
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  const Text(
                    "Manual Garage Control:",
                    style: TextStyle(fontSize: 18),
                  ),
                  if (!manualOpen)
                    ElevatedButton(
                      onPressed: () => _toggleManualOpen(true),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.green,
                        foregroundColor: Colors.white,
                      ),
                      child: const Text("Open"),
                    ),
                  if (manualOpen)
                    ElevatedButton(
                      onPressed: () => _toggleManualOpen(false),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.red,
                        foregroundColor: Colors.white,
                      ),
                      child: const Text("Close"),
                    ),
                ],
              ),
            ] else ...[
              const SizedBox(height: 20),
              const Center(
                child: Text(
                  "When the camera sees your car, the garage will automatically open.",
                  textAlign: TextAlign.center,
                ),
              ),
              const SizedBox(height: 10),
              Center(
                child: Text(
                  "Garage is currently ${manualOpen == true ? 'open' : 'closed'}.",
                  textAlign: TextAlign.center,
                  style: const TextStyle(fontWeight: FontWeight.bold),
                ),
              ),
            ],
            const SizedBox(height: 20),
            Expanded(
              child: Center(
                child: Padding(
                  padding: const EdgeInsets.all(8.0),
                  child: image != ""
                      ? Image.memory(
                          base64Decode(image),
                          fit: BoxFit.contain,
                          width: 350,
                          height: 350,
                        )
                      : const SizedBox(
                          width: 350,
                          height: 350,
                          child: Icon(Icons.image, size: 200),
                        ),
                ),
              ),
            ),
            const SizedBox(height: 20),
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                ElevatedButton(
                    onPressed: () {
                      _showChangeUsernameForm();
                    },
                    child: const Text("Specify your car model"))
              ],
            ),
          ],
        ),
      ),
    );
  }
}
