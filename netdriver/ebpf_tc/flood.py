import socket
s = socket.socket(2, 1)
while 1:
    try:
        s.connect(("10.0.2.2", 80))
        s.send(b"X"*100)
    except:
        pass
