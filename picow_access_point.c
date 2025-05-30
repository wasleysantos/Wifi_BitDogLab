#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include "hardware/i2c.h"
#include "ssd1306.h"
#include "pico/time.h"

#define TCP_PORT 80
#define POLL_TIME_S 5
#define HTTP_GET "GET"
#define HTTP_RESPONSE_HEADERS "HTTP/1.1 200 OK\nContent-Length: %d\nContent-Type: text/html\nConnection: close\n\n"
#define HTTP_RESPONSE_REDIRECT "HTTP/1.1 302 Found\nLocation: http://%s/bitdoglabtest\n\n"

#define LED_RED 13
#define LED_GREEN 11
#define LED_BLUE 12
#define BUZZER 10

const uint I2C_SDA = 14;
const uint I2C_SCL = 15;
uint8_t ssd[ssd1306_buffer_length];
struct render_area frame_area;

// Flags e timer do alarme
volatile bool alarme_ativo = false;
volatile bool estado_alarme = false;
repeating_timer_t alarme_timer;

typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    bool complete;
    ip_addr_t gw;
} TCP_SERVER_T;

typedef struct TCP_CONNECT_STATE_T_ {
    struct tcp_pcb *pcb;
    int sent_len;
    char headers[128];
    char result[1024];
    int header_len;
    int result_len;
    ip_addr_t *gw;
} TCP_CONNECT_STATE_T;

void ssd1306_draw_string_scaled(uint8_t *buffer, int x, int y, const char *text, int scale) {
    while (*text) {
        for (int dx = 0; dx < scale; dx++) {
            for (int dy = 0; dy < scale; dy++) {
                ssd1306_draw_char(buffer, x + dx, y + dy, *text);
            }
        }
        x += 6 * scale;
        text++;
    }
}

void atualizar_display() {
    
    memset(ssd, 0, ssd1306_buffer_length);
    ssd1306_draw_string_scaled(ssd, 0, 0, "IIIIIIIIIIIIIIIIIIIIIIIIIII", 1);
    ssd1306_draw_string_scaled(ssd, 20, 20, "SISTEMA", 2);
    ssd1306_draw_string_scaled(ssd, 50, 35, "EM", 2);
    ssd1306_draw_string_scaled(ssd, 20, 50, "REPOUSO", 2);
    

    render_on_display(ssd, &frame_area);
}

bool alarme_callback(repeating_timer_t *rt) {
    if (!alarme_ativo) {
        gpio_put(LED_RED, 0);
        gpio_put(BUZZER, 0);
        return false;
    }

    estado_alarme = !estado_alarme;
    gpio_put(LED_RED, estado_alarme);
    gpio_put(BUZZER, estado_alarme);

    memset(ssd, 0, ssd1306_buffer_length);
    ssd1306_draw_string_scaled(ssd, 20, 30, "EVACUAR", 2);
    render_on_display(ssd, &frame_area);

    return true;
}

void ativar_alarme() {
    if (!alarme_ativo) {
        alarme_ativo = true;
        add_repeating_timer_ms(500, alarme_callback, NULL, &alarme_timer);
    }
}

void init_leds() {
    gpio_init(LED_RED);   gpio_set_dir(LED_RED, GPIO_OUT);   gpio_put(LED_RED, 0);
    gpio_init(LED_GREEN); gpio_set_dir(LED_GREEN, GPIO_OUT); gpio_put(LED_GREEN, 0);
    gpio_init(LED_BLUE);  gpio_set_dir(LED_BLUE, GPIO_OUT);  gpio_put(LED_BLUE, 0);
    gpio_init(BUZZER);    gpio_set_dir(BUZZER, GPIO_OUT);    gpio_put(BUZZER, 0);
}

void parse_params(const char *params) {
    int r, g, b;
    if (sscanf(params, "red=%d&green=%d&blue=%d", &r, &g, &b) == 3) {
        gpio_put(LED_RED, r);
        gpio_put(LED_GREEN, g);
        gpio_put(LED_BLUE, b);
    } else {
        if (strstr(params, "red=1")) gpio_put(LED_RED, 1);
        if (strstr(params, "red=0")) gpio_put(LED_RED, 0);
        if (strstr(params, "green=1")) gpio_put(LED_GREEN, 1);
        if (strstr(params, "green=0")) gpio_put(LED_GREEN, 0);
        if (strstr(params, "blue=1")) gpio_put(LED_BLUE, 1);
        if (strstr(params, "blue=0")) gpio_put(LED_BLUE, 0);
    }

    if (strstr(params, "buzzer=1")) gpio_put(BUZZER, 1);
    if (strstr(params, "buzzer=0")) gpio_put(BUZZER, 0);

    if (strstr(params, "alarme=1")) {
        ativar_alarme();
    } else if (strstr(params, "alarme=0")) {
        alarme_ativo = false;
        gpio_put(LED_RED, 0);
        gpio_put(BUZZER, 0);
        atualizar_display();
    }

    atualizar_display();
}

int generate_html(char *result, size_t max_len) {
    bool r = gpio_get(LED_RED);
   return snprintf(result, max_len,
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body { background-color:rgb(60, 141, 168); font-family: Arial, sans-serif; margin: 0; padding: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100vh; }"
    "h2 { color: #333; margin-top: 30px; text-align: center; font-size: 1.8em; }"
    ".status { margin: 20px; padding: 20px; background: white; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); width: 90%%; max-width: 400px; }"
    ".btn { display: block; width: 90%%; padding: 15px; margin: 10px 0; font-size: 1.1em; font-weight: bold; border: none; border-radius: 8px; cursor: pointer; transition: 0.3s; }"
    ".btn-on { background-color:rgb(216, 34, 14); color: white; }"
    ".btn-off { background-color:rgb(17, 134, 66); color: white; }"
    ".btn:hover { opacity: 0.85; }"
    "</style>"
    "</head>"
    "<body>"
    "<h2>Alarme de Emergencia</h2>"
    "<div class='status'>"
    "<a class='btn btn-on' href='?alarme=1'>Ativar Alarme</a>"
    "<a class='btn btn-off' href='?alarme=0'>Desligar Alarme</a>"
    "</div>"
    "</body>"
    "</html>"
);
}

int handle_request(const char *request, const char *params, char *result, size_t max_len) {
    if (strncmp(request, "/bitdoglabtest", 8) == 0) {
        if (params) parse_params(params);
        return generate_html(result, max_len);
    }
    return 0;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    if (!p) return tcp_close(pcb);
    pbuf_copy_partial(p, con_state->headers, sizeof(con_state->headers) - 1, 0);
    char *request_line = strtok(con_state->headers, "\r\n");
    char *method = strtok(request_line, " ");
    char *url = strtok(NULL, " ");
    char *params = strchr(url, '?');
    if (params) { *params = 0; params++; }
    con_state->result_len = handle_request(url, params, con_state->result, sizeof(con_state->result));
    if (con_state->result_len > 0)
        con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS, con_state->result_len);
    else {
        con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_REDIRECT, ipaddr_ntoa(con_state->gw));
        con_state->result_len = 0;
    }
    con_state->sent_len = 0;
    tcp_write(pcb, con_state->headers, con_state->header_len, 0);
    if (con_state->result_len > 0) tcp_write(pcb, con_state->result, con_state->result_len, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return tcp_close(pcb);
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    TCP_CONNECT_STATE_T *con_state = calloc(1, sizeof(TCP_CONNECT_STATE_T));
    con_state->pcb = client_pcb;
    con_state->gw = &state->gw;
    tcp_arg(client_pcb, con_state);
    tcp_recv(client_pcb, tcp_server_recv);
    return ERR_OK;
}

static bool tcp_server_open(TCP_SERVER_T *state, const char *ap_name) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return false;
    if (tcp_bind(pcb, IP_ANY_TYPE, TCP_PORT)) return false;
    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);
    printf("Acesse: http://%s/bitdoglabtest\n", ap_name);
    return true;
}

void key_pressed_func(void *param) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)param;
    int key = getchar_timeout_us(0);
    if (key == 'd' || key == 'D') {
        cyw43_arch_lwip_begin();
        cyw43_arch_disable_ap_mode();
        cyw43_arch_lwip_end();
        state->complete = true;
    }
}

int main() {
    stdio_init_all();
    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    cyw43_arch_init();
    stdio_set_chars_available_callback(key_pressed_func, state);

    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init();

    frame_area.start_column = 0;
    frame_area.end_column = ssd1306_width - 1;
    frame_area.start_page = 0;
    frame_area.end_page = ssd1306_n_pages - 1;
    calculate_render_area_buffer_length(&frame_area);

    memset(ssd, 0, ssd1306_buffer_length);
    atualizar_display();
    init_leds();

    const char *ap_name = "BitDogLab Wasley";
    const char *password = "12345678";
    cyw43_arch_enable_ap_mode(ap_name, password, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t mask;
    IP4_ADDR(&state->gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, &state->gw, &mask);

    dns_server_t dns_server;
    dns_server_init(&dns_server, &state->gw);

    if (!tcp_server_open(state, "192.168.4.1")) return 1;

    state->complete = false;
    while (!state->complete) {
        sleep_ms(1000);
    }

    cyw43_arch_deinit();
    return 0;
}
