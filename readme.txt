



Server commands

// compile
gcc mtp.c mongoose/mongoose.c -o mtp -I./mongoose -pthread -ljson-c
gcc fmp.c -o fmp -lcurl -ljson-c
gcc -o scanner scanner.c -ljson-c -lwebsockets -lpthread -lm


// make executable
chmod -x {file}


//Create a Systemd Service
 nano /etc/systemd/system/mtp.service


// Enable and Start the Service systemctl
systemctl daemon-reload
systemctl enable mtp
systemctl start fmp
systemctl stop mtp
systemctl restart mtp
systemctl status fmp
journalctl -fu fmp
tail -f /var/log/scanner.log
journalctl -u scanner --priority=3 //  only error logs


// To run them immediately

Start MTP WebSocket Server
 systemctl start mtp.service
 systemctl start fmp.service

// Check Logs
 journalctl -u mtp -f

// Or view specific log files:
cat /var/log/mtp.log
cat /var/log/mtp_error.log


// 1 Create the fmp Systemd Service
 nano /etc/systemd/system/fmp.service
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
 nano /etc/systemd/system/fmp.timer
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
 nano /etc/systemd/system/fmp-stop.service
```
[Unit]
Description=Stop FMP program

[Service]
ExecStart=/bin/systemctl stop fmp
User=root
```

// Create a Timer to Stop
 nano /etc/systemd/system/fmp-stop.timer
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
 systemctl daemon-reload
 systemctl enable fmp.timer fmp-stop.timer
 systemctl start fmp.timer fmp-stop.timer

// 6 Verify
systemctl list-timers --all
 systemctl status fmp
