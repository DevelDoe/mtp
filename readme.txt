



Server commands

// compile
gcc mtp.c mongoose/mongoose.c -o mtp -I./mongoose -pthread -ljson-c
gcc fmp.c -o fmp -lcurl -ljson-c
gcc -o scanner scanner.c -ljson-c -lwebsockets -lpthread -lm


// make executable
chmod -x {file}


//Create a Systemd Service
sudo nano /etc/systemd/system/mtp.service


// Enable and Start the Service systemctl
sudo systemctl daemon-reload
sudo systemctl enable mtp
sudo systemctl start mtp
sudo systemctl status mtp

sudo systemctl restart mtp
sudo systemctl stop mtp
sudo systemctl start fmp
sudo systemctl status fmp

// To run them immediately

Start MTP WebSocket Server
sudo systemctl start mtp.service
sudo systemctl start fmp.service

// Check Logs
sudo journalctl -u mtp -f

// Or view specific log files:
cat /var/log/mtp.log
cat /var/log/mtp_error.log


// 1 Create the fmp Systemd Service
sudo nano /etc/systemd/system/fmp.service
```
[Unit]
Description=FMP Program (Runs at 3:50 AM - 5:00 PM)
After=network.target

[Service]
ExecStart=/root/mtp/fmp
Restart=always
User=root
WorkingDirectory=/root/mtp
StandardOutput=append:/var/log/fmp.log
StandardError=append:/var/log/fmp_error.log
```

//  2 Create a Timer to Start fmp at 3:50 AM
sudo nano /etc/systemd/system/fmp.timer
```
[Unit]
Description=Timer to start and stop FMP program

[Timer]
OnCalendar=*-*-* 03:50:00
Unit=fmp.service
Persistent=true

[Install]
WantedBy=timers.target
```

// 3 Create a Service to Stop
sudo nano /etc/systemd/system/fmp-stop.service
```
[Unit]
Description=Stop FMP program

[Service]
ExecStart=/bin/systemctl stop fmp
User=root
```

// Create a Timer to Stop
sudo nano /etc/systemd/system/fmp-stop.timer
```
[Unit]
Description=Timer to stop FMP program

[Timer]
OnCalendar=*-*-* 17:00:00
Unit=fmp-stop.service
Persistent=true

[Install]
WantedBy=timers.target
```

// 5 Enable and Start the Timers
sudo systemctl daemon-reload
sudo systemctl enable fmp.timer fmp-stop.timer
sudo systemctl start fmp.timer fmp-stop.timer

// 6 Verify
systemctl list-timers --all
sudo systemctl status fmp
