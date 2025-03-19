import 'package:firebase_database/firebase_database.dart';

class FirebaseService {
  final DatabaseReference _dbRef = FirebaseDatabase.instance.ref('garage');

  Stream<String> detectedCarStream() {
    return _dbRef.child('result').onValue.map((event) {
      final String detectedCar = event.snapshot.value.toString();
      return detectedCar;
    });
  }

  Stream<String> imageStream() {
    return _dbRef.child('image').onValue.map((event) {
      final String image = event.snapshot.value.toString();
      return image;
    });
  }

  Future<void> setAutoMode(bool isAutoMode) async {
    await _dbRef.child('manual').set(!isAutoMode);
  }

  Future<void> setManualOpen(bool open) async {
    await _dbRef.child('manualopen').set(open);
  }

  Future<void> setNewCar(String newCar) async {
    await _dbRef.child('ownercar').set(newCar);
  }

  Stream<bool> autoModeStream() {
    return _dbRef.child('manual').onValue.map((event) {
      return event.snapshot.value == false;
    });
  }

  Stream<bool> manualOpenStream() {
    return _dbRef.child('manualopen').onValue.map((event) {
      return event.snapshot.value == true;
    });
  }
}
