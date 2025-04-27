from flask import Flask, request, render_template, redirect, url_for, flash, session
from flask_login import LoginManager, UserMixin, login_user, login_required, logout_user, current_user
import serial
import hashlib
import os
import time
from functools import wraps

app = Flask(__name__)
# Use a secure random key instead of hardcoded value
app.secret_key = os.environ.get('SECRET_KEY', os.urandom(24).hex())

# Serial port configuration with better error handling
ser = None
try:
    ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
    print("Successfully connected to serial port.")
except serial.SerialException as e:
    print(f"Error: Could not open serial port. {str(e)}")
    print("The application will start, but message sending will be disabled.")

# User class for Flask-Login
class User(UserMixin):
    def __init__(self, id):
        self.id = id

# More secure user database with better password hashing
# In production, use a proper database and password hashing library like bcrypt
def hash_password(password, salt=None):
    if salt is None:
        salt = os.urandom(16).hex()
    hashed = hashlib.pbkdf2_hmac('sha256', password.encode(), salt.encode(), 100000)
    return f"{salt}${hashed.hex()}"

def verify_password(stored_password, provided_password):
    salt, hashed = stored_password.split('$')
    return hash_password(provided_password, salt).split('$')[1] == hashed

# Initialize with a more secure password
salt = os.urandom(16).hex()
hashed_pw = hash_password('password1', salt)
users = {'user1': {'password': hashed_pw}}

login_manager = LoginManager()
login_manager.init_app(app)
login_manager.login_view = 'login'
login_manager.login_message = 'Please log in to access this page.'

@login_manager.user_loader
def load_user(user_id):
    return User(user_id) if user_id in users else None

# Rate limiting decorator to prevent abuse
def rate_limit(min_interval=2):
    def decorator(f):
        last_request_time = {}
        @wraps(f)
        def decorated_function(*args, **kwargs):
            user_id = current_user.id if current_user.is_authenticated else request.remote_addr
            current_time = time.time()
            if user_id in last_request_time:
                time_since_last = current_time - last_request_time[user_id]
                if time_since_last < min_interval:
                    flash(f'Please wait {min_interval-time_since_last:.1f} seconds before sending another message.', 'warning')
                    return redirect(url_for('send_message'))
            last_request_time[user_id] = current_time
            return f(*args, **kwargs)
        return decorated_function
    return decorator

@app.route('/')
def index():
    return redirect(url_for('login'))

@app.route('/login', methods=['GET', 'POST'])
def login():
    if current_user.is_authenticated:
        return redirect(url_for('send_message'))
        
    if request.method == 'POST':
        username = request.form['username']
        password = request.form['password']
        
        if username in users and verify_password(users[username]['password'], password):
            user = User(username)
            login_user(user, remember=True)
            next_page = request.args.get('next')
            flash('Login successful!', 'success')
            return redirect(next_page or url_for('send_message'))
        else:
            flash('Invalid username or password', 'danger')
    
    return render_template('login.html')

@app.route('/send_message', methods=['GET', 'POST'])
@login_required
@rate_limit(2)  # Limit to one message every 2 seconds
def send_message():
    message_sent = False
    error_message = None
    arduino_response = None
    
    if request.method == 'POST':
        address = request.form['address']
        message = request.form['message']
        
        # Improved input validation
        if not address.isdigit():
            error_message = 'Address must contain only numbers'
        elif int(address) <= 0:
            error_message = 'Address must be a positive number'
        elif len(message) > 40:
            error_message = 'Message cannot exceed 40 characters'
        elif len(message) == 0:
            error_message = 'Message cannot be empty'
        else:
            # Send to RP2040
            if ser is not None and ser.is_open:
                try:
                    command = f"{address}:{message}\n"
                    ser.write(command.encode())
                    ser.flush()
                    
                    # Wait for and read the response from Arduino
                    # Set a timeout for reading the response
                    timeout = time.time() + 5  # 5 second timeout
                    response_data = ""
                    
                    while time.time() < timeout:
                        if ser.in_waiting > 0:
                            line = ser.readline().decode('utf-8').strip()
                            response_data += line
                            if "success" in line.lower() or "error" in line.lower() or "complete" in line.lower():
                                break
                    
                    if response_data:
                        arduino_response = response_data
                        message_sent = "error" not in response_data.lower()
                        if message_sent:
                            flash('Message sent successfully!', 'success')
                        else:
                            flash(f'Error: {response_data}', 'danger')
                    else:
                        arduino_response = "No response from transmitter"
                        flash('Message sent but no confirmation received', 'warning')
                        message_sent = True
                        
                except serial.SerialException as e:
                    error_message = f'Error sending message: {str(e)}'
            else:
                error_message = 'Serial connection is not available'
                
        if error_message:
            flash(error_message, 'danger')
    
    return render_template('send_message.html', message_sent=message_sent, arduino_response=arduino_response)

@app.route('/logout')
@login_required
def logout():
    logout_user()
    flash('You have been logged out', 'info')
    return redirect(url_for('login'))

# Error handlers
@app.errorhandler(404)
def page_not_found(e):
    return render_template('error.html', error_code=404, message="Page not found"), 404

@app.errorhandler(500)
def internal_server_error(e):
    return render_template('error.html', error_code=500, message="Internal server error"), 500

if __name__ == '__main__':
    # In production, use a proper WSGI server and set debug=False
    app.run(host='0.0.0.0', port=9000, debug=False)