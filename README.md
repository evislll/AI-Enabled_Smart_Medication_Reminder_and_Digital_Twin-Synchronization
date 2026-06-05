# AI Smart Medication Reminder & Digital Twin Synchronization
--- 
- An embedded, closed-loop medication management system built on the **NXP FRDM-K66F** that reminds, dispenses, verifies intake, and adapts — synchronized in real time to a web-based digital twin.
- Traditional reminder systems notify but cannot verify. This system closes the loop — a pill is scheduled, dispensed by servo, sensed by a load cell, and the result drives both local behavior and a live remote dashboard. No manual confirmation required.

---
## Architechture
![image alt](https://github.com/evislll/AI-Enabled_Smart_Medication_Reminder_and_Digital_Twin-Synchronization/blob/03895f864b36bc311612d45342d0c0c16619f879/System%20Flow%20Diagram.png)
![image alt]()
---
##  Hardware

| Component | Interface | Role |
|---|---|---|
| NXP FRDM-K66F (ARM Cortex-M4) | — | Main MCU |
| DS3231 RTC | I2C | Accurate dose scheduling |
| HX711 + Load Cell | GPIO / IRQ | Dispense & intake weight sensing |
| Servo Motor | PWM | Pill dispensing |
| SSD1306 OLED | I2C | On-device status display |
| Buzzer | GPIO | Audible alerts |
| Push Buttons | GPIO / IRQ | ACK / Snooze input |

---

## Key Features

### Intelligent Reminder System
Reminders follow a multi-stage flow driven by the state machine. High-risk medications enter a `ST_PREALERT` stage before the main reminder fires. If the user doesn't respond, the system transitions to `ST_POST_TIMEOUT_VERIFY` and applies a grace period before marking a dose missed. Snooze is handled with a configurable delay that re-enters the reminder stage without resetting the risk score.

### AI Risk-Adaptive Behavior
Each medication maintains a dynamic risk score updated after every event — missed doses, failed dispenses, unacknowledged reminders, and snooze frequency all feed the score. The score maps directly to reminder interval:

| Risk Level | Reminder Interval |
|---|---|
| LOW | 10 s |
| MEDIUM | 8 s |
| HIGH | 5 s |

Risk state is refreshed **before** applying any policy, ensuring decisions always reflect the latest data rather than stale cached values.
![image alt](https://github.com/evislll/AI-Enabled_Smart_Medication_Reminder_and_Digital_Twin-Synchronization/blob/2dfdd91c1c8ae4b3d87bd8c1e642eb7154c5bb8d/Individual%20Medication%20Risk%20Line%20Chart.png)
![image alt](https://github.com/evislll/AI-Enabled_Smart_Medication_Reminder_and_Digital_Twin-Synchronization/blob/6a51bfd13e6b88a80d4e863613bb0903d3bff068/diagram.png)
### Closed-Loop Dispense Verification
The servo dispenses only after the load cell records a stable pre-dispense baseline. After actuation, the weight delta is compared against a threshold. Success and failure are both logged locally and posted to the server — no assumption is made about whether a pill was dispensed.

### Ethernet Communication (lwIP)
The MCU communicates with the Flask server over TCP/IP using the lwIP stack. All transmissions use **non-blocking sockets** with retry logic to prevent any single network call from stalling a FreeRTOS task. Four REST endpoints handle the full communication surface:

| Endpoint | Purpose |
|---|---|
| `POST /api/events` | Log reminder and dose events |
| `POST /api/twin/update` | Push live MCU state to digital twin |
| `POST /api/dispense_verification` | Report dispense success/failure |
| `GET /api/command` | Poll for remote commands (ACK, SNOOZE, DISPENSE, TAKEN) |

### Digital Twin Dashboard
The dashboard mirrors the live MCU state and accepts remote commands that feed back into the embedded state machine. Artificial weight injection is supported for testing without physical hardware.
Displays:
- Reminder state
- AI risk level
- Servo state
- Weight readings
Supports remote commands:
- ACK
- SNOOZE
- DISPENSE
- TAKEN
---
![image alt](https://github.com/evislll/AI-Enabled_Smart_Medication_Reminder_and_Digital_Twin-Synchronization/blob/c786eecc75afcd595d81fa1754fa86c1cd8fbe6d/Digital%20Twin%20Dashboard%20Implementation.png)

###  Dual Logging & Reboot Recovery
Events are written to **onboard flash** and posted to the SQLite server database. On reboot, the flash log is replayed in order to reconstruct risk scores and reminder state — the system continues from where it left off with no data loss.

---

##  Implementation Details

### Threading 
The firmware uses multiple concurrent FreeRTOS tasks with distinct responsibilities and priorities:

- **ReminderTask** — drives the state machine, handles timing, and triggers buzzer/OLED output
- **NetworkTask** — manages all lwIP socket communication; runs non-blocking to avoid delaying other tasks
- **SensorTask** — polls the HX711 load cell and publishes weight readings to a shared queue
- **LogTask** — writes events to flash and queues server-bound payloads

Tasks communicate through **FreeRTOS queues and semaphores** rather than shared globals, keeping data access safe without disabling the scheduler.

### Interrupt Handling
Button inputs (ACK / Snooze) are handled via **GPIO interrupts** that post directly to a FreeRTOS queue from the ISR using `xQueueSendFromISR()`. This keeps ISRs minimal — no logic runs in the interrupt context, only a queue post that unblocks the ReminderTask on the next scheduler tick.

The HX711 data-ready signal is similarly interrupt-driven, triggering a lightweight ISR that releases a semaphore to wake the SensorTask rather than polling in a busy loop.

### Optimization
The NetworkTask batches state fields into a single JSON payload per `/api/twin/update` call rather than sending one request per field. 
Socket creation failures use an **exponential backoff retry loop** to handle transient Ethernet issues without flooding the network or blocking indefinitely.
 Commands are polled rather than pushed from the server to avoid requiring the MCU to maintain a persistent open socket.

---

## State Machine

![image alt](https://github.com/evislll/AI-Enabled_Smart_Medication_Reminder_and_Digital_Twin-Synchronization/blob/57cc563b1ca84aa47d2135a26607574b3002b4b6/State%20Control.png)

---

## Server

- **Python Flask** backend with REST API
- **SQLite** database for event and risk history
- **Digital Twin dashboard** — live MCU state + remote control (ACK / SNOOZE / DISPENSE / TAKEN)
- Simulation mode: inject artificial weight values for integration testing

---


## References

[1] WHO, *Adherence to Long-Term Therapies*, 2003. [Link](https://www.paho.org/sites/default/files/WHO-Adherence-Long-Term-Therapies-Eng-2003.pdf)  
[2] Brown & Bussell, *Medication Adherence: WHO Cares?*, Mayo Clinic Proc., 2011. [Link](https://pmc.ncbi.nlm.nih.gov/articles/PMC3068890/)  
[3] Nandhakumar et al., *Digital Twin Technology in Healthcare*, 2024. [Link](https://ijgis.pubpub.org/pub/q55wlwee)


*SEA600 Course Project · Group members: Mankomal Gandhara, Ekaterina Vislova · April 2026*
#
