// =====================================================================
// EV SMARTRANGE - backend and web server in one file
// Server: http://localhost:8080
// Routes: /range, /logtrip, /mileage, /charging, /history, /clear
// =====================================================================

// ===== HEADERS AND SETTINGS =====

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <winsock2.h>

#define PORT 8080
#define HISTORY_FILE "trips.txt"
#define SAFE_RANGE_FACTOR 0.90
#define REQUEST_SIZE 4096
#define QUERY_SIZE 1024
#define MODEL_SIZE 40
#define DATE_SIZE 40
#define LINE_SIZE 512
#define MAX_TRIPS 1000
#define BODY_SIZE 65536

// One saved trip (one line in trips.txt)
typedef struct
{
    char date[DATE_SIZE];
    char model[MODEL_SIZE];
    double capacity;
    double battery;
    double consumption;
    double range;
    double safeRange;
    double distance;
    double actual;
} Trip;

// Personalised figures for one car
typedef struct
{
    int count;
    double totalKm;
    double consumption;
} Personal;


// ===== WEB HELPERS =====

// Sends every byte of the message
static void sendAll(SOCKET client, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length)
    {
        int n = send(client, data + sent, (int)(length - sent), 0);

        if (n == SOCKET_ERROR)
        {
            return;
        }
        sent += (size_t)n;
    }
}

// Sends an HTTP response: status line, headers, then the body
static void sendResponse(SOCKET client, const char *status, const char *type,
                         const char *body, size_t length)
{
    char header[512];
    int headerLength;

    headerLength = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
        status, type, (unsigned long)length);

    sendAll(client, header, (size_t)headerLength);
    sendAll(client, body, length);
}

// Sends a text message
static void sendText(SOCKET client, const char *status, const char *type, const char *text)
{
    sendResponse(client, status, type, text, strlen(text));
}

// Sends a JSON error message. The message must not contain quotes.
static void sendError(SOCKET client, const char *message)
{
    char body[256];

    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    sendText(client, "400 Bad Request", "application/json", body);
}

// Returns 1 if text ends with suffix, otherwise 0
static int endsWith(const char *text, const char *suffix)
{
    size_t textLength = strlen(text);
    size_t suffixLength = strlen(suffix);

    return textLength >= suffixLength &&
           strcmp(text + textLength - suffixLength, suffix) == 0;
}

// Turns %20 into a space, + into a space, and so on
static void urlDecode(const char *in, char *out, size_t size)
{
    size_t n = 0;

    while (*in != '\0' && n < size - 1)
    {
        if (*in == '+')
        {
            out[n++] = ' ';
            in++;
        }
        else if (*in == '%' && isxdigit((unsigned char)in[1]) &&
                 isxdigit((unsigned char)in[2]))
        {
            char hex[3];

            hex[0] = in[1];
            hex[1] = in[2];
            hex[2] = '\0';
            out[n++] = (char)strtol(hex, NULL, 16);
            in += 3;
        }
        else
        {
            out[n++] = *in++;
        }
    }
    out[n] = '\0';
}

// Finds name=value in the query text and copies the decoded value
static int getParam(const char *query, const char *name, char *out, size_t size)
{
    char key[64];
    char raw[256];
    const char *p = query;
    size_t n = 0;

    snprintf(key, sizeof(key), "%s=", name);

    while ((p = strstr(p, key)) != NULL)
    {
        if (p == query || *(p - 1) == '&')
        {
            p += strlen(key);

            while (*p != '\0' && *p != '&' && n < sizeof(raw) - 1)
            {
                raw[n++] = *p++;
            }
            raw[n] = '\0';

            urlDecode(raw, out, size);
            return 1;
        }
        p++;
    }

    return 0;
}

// Reads one number from the query text. Returns 1 if it was a valid number.
static int getNumber(const char *query, const char *name, double *value)
{
    char text[64];

    if (!getParam(query, name, text, sizeof(text)))
    {
        return 0;
    }

    return sscanf(text, "%lf", value) == 1;
}

// Keeps only letters, digits, spaces, - . _ in the model name
static void cleanModel(char *model)
{
    size_t i;
    size_t length;

    for (i = 0; model[i] != '\0'; i++)
    {
        if (!isalnum((unsigned char)model[i]) && model[i] != ' ' &&
            model[i] != '-' && model[i] != '.' && model[i] != '_')
        {
            model[i] = '_';
        }
    }

    length = strlen(model);
    while (length > 0 && model[length - 1] == ' ')
    {
        model[--length] = '\0';
    }

    if (length == 0)
    {
        strcpy(model, "Unnamed");
    }
}

// Reads the model name from the query. Missing names become "Unnamed".
static void getModel(const char *query, char *model)
{
    if (!getParam(query, "model", model, MODEL_SIZE))
    {
        model[0] = '\0';
    }
    cleanModel(model);
}

// Compares two model names, ignoring upper and lower case
static int sameModel(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
        {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

// Sends a file. Looks in the backend folder first, then in ../frontend.
static void serveFile(SOCKET client, const char *name)
{
    FILE *fp;
    long size;
    char *data;
    const char *type;
    char path[300];

    if (name[0] == '\0' || strstr(name, "..") != NULL ||
        strchr(name, '/') != NULL || strchr(name, '\\') != NULL)
    {
        sendText(client, "404 Not Found", "text/plain", "Not found");
        return;
    }

    if (endsWith(name, ".html"))
    {
        type = "text/html; charset=utf-8";
    }
    else if (endsWith(name, ".css"))
    {
        type = "text/css";
    }
    else if (endsWith(name, ".js"))
    {
        type = "application/javascript";
    }
    else
    {
        sendText(client, "404 Not Found", "text/plain", "Not found");
        return;
    }

    fp = fopen(name, "rb");
    if (fp == NULL)
    {
        snprintf(path, sizeof(path), "../frontend/%s", name);
        fp = fopen(path, "rb");
    }

    if (fp == NULL)
    {
        sendText(client, "404 Not Found", "text/plain", "Not found");
        return;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);

    data = malloc((size_t)size + 1);
    if (data == NULL)
    {
        fclose(fp);
        sendText(client, "500 Internal Server Error", "text/plain", "Out of memory");
        return;
    }

    (void)fread(data, 1, (size_t)size, fp);
    fclose(fp);

    sendResponse(client, "200 OK", type, data, (size_t)size);
    free(data);
}


// ===== BATTERY CALCULATION =====

// Energy left in the battery (kWh)
static double calculateAvailableEnergy(double capacity, double batteryPercent)
{
    return capacity * batteryPercent / 100.0;
}

// Distance the car can drive with this energy (km)
static double predictRange(double availableEnergy, double consumption)
{
    if (consumption <= 0)
    {
        return 0;
    }

    return availableEnergy / consumption;
}

// Range with a 10% margin kept back (km)
static double calculateSafeRange(double predictedRange)
{
    return predictedRange * SAFE_RANGE_FACTOR;
}


// ===== CHARGING CALCULATION =====

// Energy to add to the battery (kWh)
static double calculateEnergyNeeded(double capacity, double currentLevel, double targetLevel)
{
    return capacity * (targetLevel - currentLevel) / 100.0;
}

// Time to charge in decimal hours
static double calculateChargingHours(double energyNeeded, double chargerPower)
{
    if (chargerPower <= 0)
    {
        return 0;
    }

    return energyNeeded / chargerPower;
}

// Turns decimal hours into whole hours and minutes
static void splitHoursAndMinutes(double decimalHours, int *hours, int *minutes)
{
    int h = (int)decimalHours;
    int m = (int)((decimalHours - h) * 60 + 0.5);

    if (m == 60)
    {
        h = h + 1;
        m = 0;
    }

    *hours = h;
    *minutes = m;
}


// ===== TRIP HISTORY =====

// Writes the current date and time as dd-mm-yyyy hh:mm
static void currentDate(char *out, size_t size)
{
    time_t now = time(NULL);

    strftime(out, size, "%d-%m-%Y %H:%M", localtime(&now));
}

// Adds one trip as a line in trips.txt. Returns 1 on success, 0 on failure.
static int saveTrip(const Trip *t)
{
    FILE *fp = fopen(HISTORY_FILE, "a");

    if (fp == NULL)
    {
        return 0;
    }

    fprintf(fp, "%s,%s,%g,%g,%g,%.2f,%.2f,%.3f,%.6f\n",
            t->date, t->model, t->capacity, t->battery, t->consumption,
            t->range, t->safeRange, t->distance, t->actual);

    fclose(fp);
    return 1;
}

// Reads one line of trips.txt. Accepts the 9-field format and the old 6-field format.
static int parseTrip(char *line, Trip *t)
{
    char *field[9];
    int count = 0;
    int b;
    char *token = strtok(line, ",\r\n");

    while (token != NULL && count < 9)
    {
        field[count++] = token;
        token = strtok(NULL, ",\r\n");
    }

    if (count != 6 && count != 9)
    {
        return 0;
    }

    memset(t, 0, sizeof(*t));
    strncpy(t->date, field[0], DATE_SIZE - 1);

    b = (count == 9) ? 1 : 0;

    if (count == 9)
    {
        strncpy(t->model, field[1], MODEL_SIZE - 1);
    }
    else
    {
        strcpy(t->model, "Unnamed");
    }

    t->capacity = atof(field[1 + b]);
    t->battery = atof(field[2 + b]);
    t->consumption = atof(field[3 + b]);
    t->range = atof(field[4 + b]);
    t->safeRange = atof(field[5 + b]);

    if (count == 9)
    {
        t->distance = atof(field[7]);
        t->actual = atof(field[8]);
    }

    return 1;
}

// Reads all trips from trips.txt. Returns how many were read.
static int loadTrips(Trip *list, int max)
{
    FILE *fp = fopen(HISTORY_FILE, "r");
    char line[LINE_SIZE];
    int count = 0;

    if (fp == NULL)
    {
        return 0;
    }

    while (count < max && fgets(line, sizeof(line), fp) != NULL)
    {
        if (parseTrip(line, &list[count]))
        {
            count++;
        }
    }

    fclose(fp);
    return count;
}

// Works out the personal consumption for one car from its logged trips
static void personalFor(const char *model, Personal *p)
{
    static Trip trips[MAX_TRIPS];
    int total = loadTrips(trips, MAX_TRIPS);
    double weighted = 0.0;
    int i;

    p->count = 0;
    p->totalKm = 0.0;
    p->consumption = 0.0;

    for (i = 0; i < total; i++)
    {
        if (trips[i].actual > 0 && trips[i].distance > 0 &&
            sameModel(model, trips[i].model))
        {
            weighted += trips[i].actual * trips[i].distance;
            p->totalKm += trips[i].distance;
            p->count++;
        }
    }

    if (p->totalKm > 0)
    {
        p->consumption = weighted / p->totalKm;
    }
}


// ===== API ROUTES =====

// /range?model=Tata%20Nexon&capacity=60&battery=75&consumption=0.15
static void handleRange(SOCKET client, const char *query)
{
    char model[MODEL_SIZE];
    double capacity = 0.0, battery = 0.0, consumption = 0.0;
    double energy, range, safeRange;
    double personalRange = 0.0, personalSafe = 0.0;
    Personal personal;
    Trip trip;
    int saved;
    char body[512];

    getModel(query, model);

    if (!getNumber(query, "capacity", &capacity) || capacity <= 0 ||
        !getNumber(query, "battery", &battery) || battery < 0 || battery > 100 ||
        !getNumber(query, "consumption", &consumption) || consumption <= 0)
    {
        sendError(client, "Please enter valid values in all fields.");
        return;
    }

    energy = calculateAvailableEnergy(capacity, battery);
    range = predictRange(energy, consumption);
    safeRange = calculateSafeRange(range);

    personalFor(model, &personal);
    if (personal.count > 0)
    {
        personalRange = predictRange(energy, personal.consumption);
        personalSafe = calculateSafeRange(personalRange);
    }

    memset(&trip, 0, sizeof(trip));
    currentDate(trip.date, sizeof(trip.date));
    strcpy(trip.model, model);
    trip.capacity = capacity;
    trip.battery = battery;
    trip.consumption = consumption;
    trip.range = range;
    trip.safeRange = safeRange;
    saved = saveTrip(&trip);

    snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"energy\":%.2f,\"range\":%.2f,\"safeRange\":%.2f,"
        "\"saved\":%s,\"tripsUsed\":%d,\"personalConsumption\":%.4f,"
        "\"personalRange\":%.2f,\"personalSafeRange\":%.2f}",
        model, energy, range, safeRange, saved ? "true" : "false",
        personal.count, personal.consumption, personalRange, personalSafe);

    sendText(client, "200 OK", "application/json", body);
}

// /logtrip?model=Tata%20Nexon&capacity=40&start=80&end=60&distance=100
static void handleLogTrip(SOCKET client, const char *query)
{
    char model[MODEL_SIZE];
    double capacity = 0.0, start = 0.0, end = 0.0, distance = 0.0;
    double usedEnergy, actual, startEnergy, range;
    Personal personal;
    Trip trip;
    char body[512];

    getModel(query, model);

    if (!getNumber(query, "capacity", &capacity) || capacity <= 0 ||
        !getNumber(query, "start", &start) || start < 0 || start > 100 ||
        !getNumber(query, "end", &end) || end < 0 || end > 100 ||
        !getNumber(query, "distance", &distance) || distance <= 0)
    {
        sendError(client, "Please enter valid values in all fields.");
        return;
    }

    if (end >= start)
    {
        sendError(client, "End battery must be lower than start battery.");
        return;
    }

    usedEnergy = capacity * (start - end) / 100.0;
    actual = usedEnergy / distance;

    if (actual < 0.001 || actual > 10.0)
    {
        sendError(client, "These numbers do not look realistic. Check battery, distance and capacity.");
        return;
    }

    startEnergy = calculateAvailableEnergy(capacity, start);
    range = predictRange(startEnergy, actual);

    memset(&trip, 0, sizeof(trip));
    currentDate(trip.date, sizeof(trip.date));
    strcpy(trip.model, model);
    trip.capacity = capacity;
    trip.battery = start;
    trip.consumption = actual;
    trip.range = range;
    trip.safeRange = calculateSafeRange(range);
    trip.distance = distance;
    trip.actual = actual;

    if (!saveTrip(&trip))
    {
        sendError(client, "Could not save the trip to history.");
        return;
    }

    personalFor(model, &personal);

    snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"usedEnergy\":%.2f,\"actualConsumption\":%.4f,"
        "\"kmPerKwh\":%.2f,\"tripsUsed\":%d,\"personalConsumption\":%.4f}",
        model, usedEnergy, actual, 1.0 / actual,
        personal.count, personal.consumption);

    sendText(client, "200 OK", "application/json", body);
}

// /charging?capacity=40&current=20&target=80&power=7
static void handleCharging(SOCKET client, const char *query)
{
    double capacity = 0.0, current = 0.0, target = 0.0, power = 0.0;
    double energyNeeded, decimalHours;
    int hours, minutes;
    char body[256];

    if (!getNumber(query, "capacity", &capacity) || capacity <= 0 ||
        !getNumber(query, "current", &current) || current < 0 || current > 100 ||
        !getNumber(query, "target", &target) || target < 0 || target > 100 ||
        !getNumber(query, "power", &power) || power <= 0)
    {
        sendError(client, "Please enter valid values in all fields.");
        return;
    }

    if (target <= current)
    {
        sendError(client, "Target battery must be higher than current battery.");
        return;
    }

    energyNeeded = calculateEnergyNeeded(capacity, current, target);
    decimalHours = calculateChargingHours(energyNeeded, power);
    splitHoursAndMinutes(decimalHours, &hours, &minutes);

    snprintf(body, sizeof(body),
             "{\"energyNeeded\":%.2f,\"hours\":%d,\"minutes\":%d}",
             energyNeeded, hours, minutes);

    sendText(client, "200 OK", "application/json", body);
}

// /mileage?model=Tata%20Nexon
static void handleMileage(SOCKET client, const char *query)
{
    char model[MODEL_SIZE];
    Personal personal;
    double kmPerKwh = 0.0;
    char body[512];

    getModel(query, model);
    personalFor(model, &personal);

    if (personal.consumption > 0)
    {
        kmPerKwh = 1.0 / personal.consumption;
    }

    snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"tripsUsed\":%d,\"totalKm\":%.2f,"
        "\"personalConsumption\":%.4f,\"kmPerKwh\":%.2f}",
        model, personal.count, personal.totalKm,
        personal.consumption, kmPerKwh);

    sendText(client, "200 OK", "application/json", body);
}

// /history returns all saved trips as a JSON list
static void handleHistory(SOCKET client)
{
    static Trip trips[MAX_TRIPS];
    static char body[BODY_SIZE];
    int total = loadTrips(trips, MAX_TRIPS);
    size_t used = 1;
    int i;

    body[0] = '[';
    body[1] = '\0';

    for (i = 0; i < total; i++)
    {
        int written = snprintf(body + used, BODY_SIZE - used,
            "%s{\"date\":\"%s\",\"model\":\"%s\",\"capacity\":%g,\"battery\":%g,"
            "\"consumption\":%g,\"range\":%.2f,\"safeRange\":%.2f,"
            "\"distance\":%.2f,\"actual\":%.4f}",
            i > 0 ? "," : "", trips[i].date, trips[i].model,
            trips[i].capacity, trips[i].battery, trips[i].consumption,
            trips[i].range, trips[i].safeRange, trips[i].distance,
            trips[i].actual);

        if (written < 0 || (size_t)written >= BODY_SIZE - used - 2)
        {
            break;
        }
        used += (size_t)written;
    }

    body[used++] = ']';
    body[used] = '\0';

    sendText(client, "200 OK", "application/json", body);
}

// /clear deletes trips.txt
static void handleClear(SOCKET client)
{
    if (remove(HISTORY_FILE) == 0)
    {
        sendText(client, "200 OK", "application/json", "{\"ok\":true}");
    }
    else
    {
        sendText(client, "200 OK", "application/json",
                 "{\"ok\":false,\"message\":\"No saved trips to clear.\"}");
    }
}


// ===== REQUEST HANDLER =====

// Reads one browser request and sends the matching reply
static void handleClient(SOCKET client)
{
    char request[REQUEST_SIZE];
    char method[16];
    char target[1024];
    char query[QUERY_SIZE] = "";
    char *question;
    int received;

    received = recv(client, request, REQUEST_SIZE - 1, 0);
    if (received <= 0)
    {
        return;
    }
    request[received] = '\0';

    if (sscanf(request, "%15s %1023s", method, target) != 2)
    {
        sendText(client, "400 Bad Request", "text/plain", "Bad request");
        return;
    }

    if (strcmp(method, "GET") != 0)
    {
        sendText(client, "405 Method Not Allowed", "text/plain", "Only GET is supported");
        return;
    }

    question = strchr(target, '?');
    if (question != NULL)
    {
        strncpy(query, question + 1, QUERY_SIZE - 1);
        *question = '\0';
    }

    if (strcmp(target, "/range") == 0)
    {
        handleRange(client, query);
    }
    else if (strcmp(target, "/logtrip") == 0)
    {
        handleLogTrip(client, query);
    }
    else if (strcmp(target, "/mileage") == 0)
    {
        handleMileage(client, query);
    }
    else if (strcmp(target, "/charging") == 0)
    {
        handleCharging(client, query);
    }
    else if (strcmp(target, "/history") == 0)
    {
        handleHistory(client);
    }
    else if (strcmp(target, "/clear") == 0)
    {
        handleClear(client);
    }
    else if (strcmp(target, "/") == 0)
    {
        serveFile(client, "index.html");
    }
    else
    {
        serveFile(client, target + 1);
    }
}


// ===== MAIN PROGRAM =====

int main(void)
{
    WSADATA wsa;
    SOCKET server;
    SOCKET client;
    struct sockaddr_in address;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Could not start networking.\n");
        return 1;
    }

    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET)
    {
        printf("Could not create a socket.\n");
        WSACleanup();
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(PORT);

    if (bind(server, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR)
    {
        printf("Could not use port %d. Is another program using it?\n", PORT);
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (listen(server, 5) == SOCKET_ERROR)
    {
        printf("Could not start listening.\n");
        closesocket(server);
        WSACleanup();
        return 1;
    }

    printf("EV SmartRange is running.\n");
    printf("Open http://localhost:%d in your browser.\n", PORT);
    printf("Keep this window open. Press Ctrl + C to stop.\n");

    while (1)
    {
        client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET)
        {
            continue;
        }

        handleClient(client);
        closesocket(client);
    }

    return 0;
}