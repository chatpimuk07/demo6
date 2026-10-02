import json
import socket
import sys
from PyQt5.QtCore import QThread, pyqtSignal
from PyQt5.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)


# --- Thread สำหรับจัดการเชื่อมต่อ TCP ค้างไว้แบบถาวร (Persistent Connection) ---
class PersistentClientThread(QThread):
  response_received = pyqtSignal(str)
  connection_status = pyqtSignal(bool)

  def __init__(self, ip, port):
    super().__init__()
    self.ip = ip
    self.port = port
    self.running = True
    self.sock = None
    self.queue_payload = None
    # >>> FIX: buffer สะสมข้อมูลดิบ เผื่อ recv() ได้ข้อมูลมาไม่ครบบรรทัดในครั้งเดียว
    self._recv_buffer = b""

  def run(self):
    try:
      self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
      self.sock.settimeout(5)  # กันไม่ให้ recv ค้างตลอดไปถ้า ESP32 ไม่ตอบ
      self.sock.connect((self.ip, self.port))
      self.sock.settimeout(None)
      self.connection_status.emit(True)

      while self.running:
        if self.queue_payload:
          payload_str = json.dumps(self.queue_payload) + "\n"
          try:
            self.sock.sendall(payload_str.encode("utf-8"))
          except OSError as e:
            self.response_received.emit(json.dumps({"status": "error", "message": f"send failed: {e}"}))
            break
          self.queue_payload = None

          # >>> FIX: อ่านทีละ chunk แล้วสะสมไว้ จนกว่าจะเจอ '\n' ครบหนึ่งบรรทัด
          # (เดิมใช้ recv(1024) ครั้งเดียวแล้วสมมติว่าได้ข้อความครบ อาจพลาดถ้า JSON ยาว/มาไม่ครบ)
          line = self._read_line()
          if line is not None:
            self.response_received.emit(line)
          else:
            # การเชื่อมต่อถูกปิดจากฝั่งเซิร์ฟเวอร์
            self.response_received.emit(json.dumps({"status": "error", "message": "connection closed by server"}))
            break
        else:
          self.msleep(50)  # พักรอบลูปไม่ให้กิน CPU

    except Exception as e:
      self.response_received.emit(json.dumps({"status": "error", "message": str(e)}))
    finally:
      if self.sock:
        try:
          self.sock.close()
        except Exception:
          pass
      self.connection_status.emit(False)

  def _read_line(self):
    """อ่านจาก socket จนกว่าจะได้หนึ่งบรรทัด (คั่นด้วย \\n) หรือ connection ปิด (คืน None)"""
    while b"\n" not in self._recv_buffer:
      try:
        chunk = self.sock.recv(1024)
      except OSError:
        return None
      if not chunk:
        return None
      self._recv_buffer += chunk

    line, _, rest = self._recv_buffer.partition(b"\n")
    self._recv_buffer = rest
    return line.decode("utf-8", errors="replace").strip()

  def send_command(self, payload):
    self.queue_payload = payload

  def stop(self):
    self.running = False
    if self.sock:
      try:
        self.sock.close()
      except Exception:
        pass
    self.wait()


# --- หน้าต่าง GUI หลัก ---
class RPCPyQtClientApp(QWidget):

  def __init__(self):
    super().__init__()
    self.client_thread = None
    self.initUI()

  def initUI(self):
    self.setWindowTitle("ESP32 NTAG215 JSON-RPC Client")
    self.resize(480, 420)

    layout = QVBoxLayout()

    # ส่วนกรอก IP และ Port
    form_layout = QHBoxLayout()
    self.ip_input = QLineEdit("172.20.10.2")
    self.port_input = QLineEdit("8080")
    form_layout.addWidget(QLabel("IP:"))
    form_layout.addWidget(self.ip_input)
    form_layout.addWidget(QLabel("Port:"))
    form_layout.addWidget(self.port_input)
    layout.addLayout(form_layout)

    # ปุ่ม Connect / Disconnect เซิร์ฟเวอร์
    self.connect_btn = QPushButton("Connect to Server")
    self.connect_btn.clicked.connect(self.toggle_connection)
    layout.addWidget(self.connect_btn)

    # ช่องใส่ URL สำหรับเขียน NDEF ลง NTAG215
    write_layout = QHBoxLayout()
    self.data_input = QLineEdit("https://www.github.com/")
    write_layout.addWidget(QLabel("Write Text:"))
    write_layout.addWidget(self.data_input)
    layout.addLayout(write_layout)

    # ปุ่มคำสั่ง RPC (Read / Write NDEF)
    btn_layout = QHBoxLayout()
    self.read_btn = QPushButton("RPC: Read NTAG")
    self.read_btn.clicked.connect(lambda: self.send_rpc("read"))
    self.read_btn.setEnabled(False)  # ปิดการใช้งานไว้จนกว่าจะเชื่อมต่อ
    self.write_btn = QPushButton("RPC: Write NDEF")
    self.write_btn.clicked.connect(lambda: self.send_rpc("write"))
    self.write_btn.setEnabled(False)
    btn_layout.addWidget(self.read_btn)
    btn_layout.addWidget(self.write_btn)
    layout.addLayout(btn_layout)

    # พื้นที่แสดงผล Log
    layout.addWidget(QLabel("RPC Response Log:"))
    self.log_view = QTextEdit()
    self.log_view.setReadOnly(True)
    layout.addWidget(self.log_view)

    # ปุ่มเคลียร์ข้อความ
    self.clear_btn = QPushButton("Clear Log")
    self.clear_btn.clicked.connect(self.log_view.clear)
    layout.addWidget(self.clear_btn)

    self.setLayout(layout)

  def toggle_connection(self):
    if self.client_thread and self.client_thread.isRunning():
      self.client_thread.stop()
      self.connect_btn.setText("Connect to Server")
      self.read_btn.setEnabled(False)
      self.write_btn.setEnabled(False)
      self.log_view.append("Disconnected from server.")
    else:
      ip = self.ip_input.text()
      port = int(self.port_input.text())

      self.client_thread = PersistentClientThread(ip, port)
      self.client_thread.response_received.connect(self.on_response)
      self.client_thread.connection_status.connect(self.on_connection_status)
      self.client_thread.start()

  def on_connection_status(self, connected):
    if connected:
      self.connect_btn.setText("Disconnect")
      self.read_btn.setEnabled(True)
      self.write_btn.setEnabled(True)
      self.log_view.append("Connected to server successfully!")
    else:
      self.connect_btn.setText("Connect to Server")
      self.read_btn.setEnabled(False)
      self.write_btn.setEnabled(False)

  def send_rpc(self, action):
    if action == "read":
      payload = {"action": "read"}
    elif action == "write":
      payload = {"action": "write", "data": self.data_input.text()}
    else:
      return

    self.log_view.append(f"Sending RPC -> {payload}")
    if self.client_thread and self.client_thread.isRunning():
      self.client_thread.send_command(payload)

  def on_response(self, response):
    try:
      res_json = json.loads(response)
      self.log_view.append(f"Response: {res_json}")
      # >>> NEW: ถ้ามีเนื้อหา NDEF ที่ถอดได้ ให้โชว์แยกให้เห็นชัดๆ
      if "content" in res_json:
        content_type = res_json.get("content_type", "unknown")
        content = res_json.get("content", "")
        self.log_view.append(f"  -> [{content_type}] {content}")
      self.log_view.append("-" * 30)
    except Exception:
      self.log_view.append(f"Raw Response: {response}\n" + "-" * 30)


if __name__ == "__main__":
  app = QApplication(sys.argv)
  ex = RPCPyQtClientApp()
  ex.show()
  sys.exit(app.exec_())