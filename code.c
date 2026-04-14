#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <time.h>

// -------- STRUCT --------
typedef struct {
    float temp;
    int vib;
    char source[20];
    char status[20];
} sensor_data_t;

// -------- GLOBAL --------
int chid, wd_chid;

float TEMP_HIGH = 40;
float TEMP_CRITICAL = 50;

#define MAX_LOG 30
sensor_data_t log_buffer[MAX_LOG];
int log_index = 0;

pthread_mutex_t log_lock;

// -------- WATCHDOG --------
int hb1 = 0, hb2 = 0, hb3 = 0;

// -------- STATUS FUNCTION --------
void get_status(sensor_data_t *d) {

    if (d->temp > TEMP_CRITICAL && d->vib == 1)
        strcpy(d->status, "CRITICAL");

    else if (d->temp > TEMP_HIGH || d->vib == 1)
        strcpy(d->status, "WARNING");

    else
        strcpy(d->status, "NORMAL");
}

// -------- READ VIBRATION --------
int read_vibration() {

    FILE *fp = popen("gpio-bcm2711 get 20", "r");
    char buffer[100];
    int val = 0;

    if (fp != NULL) {
        if (fgets(buffer, sizeof(buffer), fp)) {
            sscanf(buffer, "%*d: %d", &val);
        }
        pclose(fp);
    }

    return val;
}

// -------- LED CONTROL --------
void control_leds(const char *source, const char *status) {

    system("gpio-bcm2711 set 17 dl");
    system("gpio-bcm2711 set 27 dl");
    system("gpio-bcm2711 set 22 dl");

    if (strcmp(status, "ALERT") == 0 ||
        strcmp(status, "CRITICAL") == 0 ||
        strcmp(status, "WARNING") == 0) {

        if (strcmp(source, "Client1") == 0)
            system("gpio-bcm2711 set 17 dh");

        else if (strcmp(source, "Client2") == 0)
            system("gpio-bcm2711 set 27 dh");

        else if (strcmp(source, "Client3") == 0)
            system("gpio-bcm2711 set 22 dh");
    }
}

// -------- LOGGING THREAD --------
void* logging_task(void* arg) {

    FILE *fp = fopen("log.txt", "a");

    while(1) {

        pthread_mutex_lock(&log_lock);

        if (log_index >= MAX_LOG) {

            float sum = 0;

            for (int i = 0; i < MAX_LOG; i++) {

                fprintf(fp, "%s,%.2f,%d,%s\n",
                        log_buffer[i].source,
                        log_buffer[i].temp,
                        log_buffer[i].vib,
                        log_buffer[i].status);

                sum += log_buffer[i].temp;
            }

            float avg = sum / MAX_LOG;

            TEMP_HIGH = avg + 5;
            TEMP_CRITICAL = avg + 10;

            printf("Updated Thresholds -> HIGH: %.2f CRITICAL: %.2f\n",
                   TEMP_HIGH, TEMP_CRITICAL);

            log_index = 0;
        }

        pthread_mutex_unlock(&log_lock);

        usleep(500000);
    }
}

// -------- WATCHDOG THREAD --------
void* watchdog_task(void* arg) {

    struct _pulse pulse;
    printf("Watchdog started\n");

    while (1) {

        MsgReceive(wd_chid, &pulse, sizeof(pulse), NULL);

        if (pulse.code == 1) hb1 = 1;
        if (pulse.code == 2) hb2 = 1;
        if (pulse.code == 3) hb3 = 1;

        static int count = 0;
        count++;

        if (count >= 5) {

            if (!hb1) printf("Client1 not responding\n");
            if (!hb2) printf("Client2 not responding\n");
            if (!hb3) printf("Client3 not responding\n");

            hb1 = hb2 = hb3 = 0;
            count = 0;
        }
    }
}

// -------- SERVER --------
void* server_task(void* arg) {

    sensor_data_t data;
    int rcvid;

    int c1_cons=0, c1_total=0;
    int c2_cons=0, c2_total=0;
    int c3_cons=0, c3_total=0;

    printf("Server started\n");

    while(1) {

        rcvid = MsgReceive(chid, &data, sizeof(data), NULL);

        if (rcvid > 0) {

            int exceeded = (data.temp > TEMP_HIGH || data.vib == 1);

            int *cons, *total;

            if (strcmp(data.source, "Client1") == 0) {
                cons = &c1_cons; total = &c1_total;
            }
            else if (strcmp(data.source, "Client2") == 0) {
                cons = &c2_cons; total = &c2_total;
            }
            else {
                cons = &c3_cons; total = &c3_total;
            }

            if (exceeded) {
                (*cons)++;
                (*total)++;
            } else {
                (*cons) = 0;
            }

            if (*cons >= 2 || *total >= 3) {

                strcpy(data.status, "ALERT");
                printf("ALERT from %s\n", data.source);

                *cons = 0;
                *total = 0;
            }
            else {
                get_status(&data);
            }

            pthread_mutex_lock(&log_lock);
            if (log_index < MAX_LOG) {
                log_buffer[log_index++] = data;
            }
            pthread_mutex_unlock(&log_lock);

            control_leds(data.source, data.status);

            printf("Server | %s | Temp: %.2f | Vib: %d | Status: %s\n",
                   data.source, data.temp, data.vib, data.status);

            MsgReply(rcvid, 0, &data, sizeof(data));
        }
    }
}

// -------- CLIENT1 (REAL SENSOR) --------
void* client1_task(void* arg) {

    sensor_data_t data;
    char buffer[100];

    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    int wd_coid = ConnectAttach(0, 0, wd_chid, _NTO_SIDE_CHANNEL, 0);

    FILE *fp = fopen("/dev/serusb1", "r");

    while(1) {

        data.temp = 30;

        if (fp != NULL && fgets(buffer, sizeof(buffer), fp)) {
            sscanf(buffer, "TEMP:%f", &data.temp);
        }

        data.vib = read_vibration();

        strcpy(data.source, "Client1");

        MsgSend(coid, &data, sizeof(data), &data, sizeof(data));
        MsgSendPulse(wd_coid, SIGEV_PULSE_PRIO_INHERIT, 1, 0);

        usleep(500000);
    }
}

// -------- CLIENT2 --------
void* client2_task(void* arg) {

    sensor_data_t data;

    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    int wd_coid = ConnectAttach(0, 0, wd_chid, _NTO_SIDE_CHANNEL, 0);

    while(1) {

        data.temp = 25 + ((rand() % 100) / 10.0);
        data.vib = rand() % 2;

        strcpy(data.source, "Client2");

        MsgSend(coid, &data, sizeof(data), &data, sizeof(data));
        MsgSendPulse(wd_coid, SIGEV_PULSE_PRIO_INHERIT, 2, 0);

        usleep(700000);
    }
}

// -------- CLIENT3 --------
void* client3_task(void* arg) {

    sensor_data_t data;

    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    int wd_coid = ConnectAttach(0, 0, wd_chid, _NTO_SIDE_CHANNEL, 0);

    while(1) {

        data.temp = 28 + (rand() % 5);
        data.vib = rand() % 2;

        strcpy(data.source, "Client3");

        MsgSend(coid, &data, sizeof(data), &data, sizeof(data));
        MsgSendPulse(wd_coid, SIGEV_PULSE_PRIO_INHERIT, 3, 0);

        usleep(600000);
    }
}

// -------- MAIN --------
int main() {

    srand(time(NULL));

    pthread_t server, logger, c1, c2, c3, watchdog;

    pthread_mutex_init(&log_lock, NULL);

    chid = ChannelCreate(0);
    wd_chid = ChannelCreate(0);

    system("gpio-bcm2711 set 17 op");
    system("gpio-bcm2711 set 27 op");
    system("gpio-bcm2711 set 22 op");
    system("gpio-bcm2711 set 20 ip");

    pthread_create(&server, NULL, server_task, NULL);
    pthread_create(&logger, NULL, logging_task, NULL);
    pthread_create(&watchdog, NULL, watchdog_task, NULL);

    pthread_create(&c1, NULL, client1_task, NULL);
    pthread_create(&c2, NULL, client2_task, NULL);
    pthread_create(&c3, NULL, client3_task, NULL);

    pthread_join(server, NULL);

    return 0;
}