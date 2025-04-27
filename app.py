from flask import Flask, request, render_template, redirect, url_for
from flask_login import LoginManager, UserMixin, login_user, login_required, logout_user
import serial
import hashlib

app = Flask(__name__)
app.secret_key = 'your_secret_key_here'  # Replace with a secure key

# Serial port configuration (adjust port if needed, e.g., '/dev/ttyUSB0')
try:
    ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
except serial.SerialException:
    print("Error: Could not open serial port. Check connection.")
    exit(1)

# User class for Flask-Login
class User(UserMixin):
    def __init__(self, id):
        self.id = id

# Dummy user database (use secure storage in production)
users = {'user1': {'password': hashlib.sha256('password1'.encode()).hexdigest()}}

login_manager = LoginManager()
login_manager.init_app(app)
login_manager.login_view = 'login'

@login_manager.user_loader
def load_user(user_id):
    return User(user_id) if user_id in users else None

@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        username = request.form['username']
        password = hashlib.sha256(request.form['password'].encode()).hexdigest()
        if username in users and users[username]['password'] == password:
            user = User(username)
            login_user(user)
            return redirect(url_for('send_message'))
        return 'Invalid credentials'
    return render_template('login.html')

@app.route('/send_message', methods=['GET', 'POST'])
@login_required
def send_message():
    if request.method == 'POST':
        address = request.form['address']
        message = request.form['message']
        # Validate input
        if not address.isdigit() or len(message) > 40:
            return 'Invalid address or message too long'
        # Send to RP2040
        try:
            ser.write(f"{address}:{message}\n".encode())
            ser.flush()
            return 'Message sent successfully!'
        except serial.SerialException:
            return 'Error sending message'
    return render_template('send_message.html')

@app.route('/logout')
@login_required
def logout():
    logout_user()
    return redirect(url_for('login'))

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)