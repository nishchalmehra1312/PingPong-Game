#include <ncurses.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>

#define WIDTH 80
#define HEIGHT 30
#define OFFSETX 10
#define OFFSETY 5
#define PORT 8080

// GameState structure now includes scores and a timestamp.
typedef struct {
    int msg_type;       // 0: game state update (e.g. ball movement), 1: paddle movement update
    int ball_x;
    int ball_y;
    int paddle1_x;
    int paddle2_x;
    int game_running;
    int score1;
    int score2;
    long long timestamp; // Timestamp in milliseconds (for paddle movement events)
} GameState;

typedef struct {
    int x, y;
    int dx, dy;
} Ball;

typedef struct {
    int x;
    int width;  // width of paddle; used for collision detection
} Paddle;

Ball ball;
Paddle paddle1, paddle2;
int game_running = 1;
int player_role; // 0 for server (Player 1), 1 for client (Player 2)
int sock;
struct sockaddr_in server_addr;

// Scores for the two players (server is Player 1, client is Player 2)
int score1 = 0, score2 = 0;

// For simple time synchronization (server acts as reference clock)
long long sync_offset = 0;  

// Function prototypes
void init();
void end_game();
void draw(WINDOW *win);
void *move_ball(void *args);
void update_paddle(int ch);
void reset_ball();
void *network_handler(void *args);
void send_game_state(int msg_type);
void receive_game_state();
void synchronize_time();
void log_latency(long long sent_timestamp);

long long get_current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long ms = tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    if(player_role == 1) {
        // Adjust client time using sync_offset so that both clocks are aligned
        ms += sync_offset;
    }
    return ms;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <server/client> [server_ip if client]\n", argv[0]);
        return 1;
    }

    // Determine role
    player_role = (strcmp(argv[1], "server") == 0) ? 0 : 1;
    
    // Initialize networking
    sock = socket(AF_INET, SOCK_STREAM, 0);
if (sock == -1) {
    perror("Socket creation failed");
    return 1;
}

// Add SO_REUSEADDR to allow immediate reuse of the port
int opt = 1;
if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
}

server_addr.sin_family = AF_INET;
server_addr.sin_port = htons(PORT);
server_addr.sin_addr.s_addr = INADDR_ANY;

if (player_role == 0) {  // Server side
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");  
        return 1;
    }
    listen(sock, 1);
    printf("Waiting for player to connect...\n");
    int client_sock = accept(sock, NULL, NULL);
    if (client_sock < 0) {
        perror("Accept failed");
        return 1;
    }
    close(sock);// CLOSING THE LISTENING FILE DESCRIOTOR 

    sock = client_sock; // NEW FILE DESCRIPTOR OF CLIENT AND SERVER
    printf("Player connected!\n");
} else {  // Client side
       if (argc < 3) {
        printf("Usage (client): %s client <server_ip>\n", argv[0]);
        return 1;
    }
    server_addr.sin_addr.s_addr = inet_addr(argv[2]);
    while (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        sleep(1);
    }
    printf("Connected to server!\n");
}
    
    // Perform simple time synchronization
    synchronize_time();
    
    // Initialize game objects
    ball = (Ball){WIDTH / 2, HEIGHT / 2, 1, 1};
    // Set paddle widths (adjust as needed; here 8 matches the drawn "========")
    paddle1 = (Paddle){WIDTH / 2 - 4, 8};
    paddle2 = (Paddle){WIDTH / 2 - 4, 8};
    
    init();
    
    pthread_t ball_thread, network_thread;
    // Only the server moves the ball.
    if (player_role == 0) {
        pthread_create(&ball_thread, NULL, move_ball, NULL);
    }
    pthread_create(&network_thread, NULL, network_handler, NULL);
    
    while (game_running) {
        int ch = getch();
        if (ch == 'q') {
            game_running = 0;
            break;
        }
        if(ch == KEY_LEFT || ch == KEY_RIGHT) {
            update_paddle(ch);
        }
        draw(stdscr);
    }
    
    pthread_join(network_thread, NULL);
    if (player_role == 0) {
        pthread_join(ball_thread, NULL);
    }
    end_game();
    close(sock);
    return 0;
}

void init() {
    initscr();
    timeout(10);
    keypad(stdscr, TRUE);
    curs_set(FALSE);
    noecho();
}

void end_game() {
    endwin();
}

void draw(WINDOW *win) {
    clear();
    // Draw scores at the top
    mvprintw(0, OFFSETX, "Player1 Score: %d   Player2 Score: %d", score1, score2);
    // Draw ball
    mvprintw(OFFSETY + ball.y, OFFSETX + ball.x, "o");
    // Draw paddles
    mvprintw(OFFSETY + HEIGHT - 4, OFFSETX + paddle1.x, "========");
    mvprintw(OFFSETY + 2, OFFSETX + paddle2.x, "========");
    refresh();
}

void *move_ball(void *args) {
    while (game_running) {
        ball.x += ball.dx;
        ball.y += ball.dy;
        
        // Check for collision with side walls
        if (ball.x <= 2 || ball.x >= WIDTH - 2) {
            ball.dx = -ball.dx;
        }
        
        // Check for collision at the bottom (Player1's paddle)
        if (ball.dy > 0 && ball.y >= HEIGHT - 5) {
            if (ball.x >= paddle1.x && ball.x <= paddle1.x + paddle1.width) {
                score1++;
                ball.dy = -ball.dy;
            } else {
                score1--;
                reset_ball();
            }
        }
        // Check for collision at the top (Player2's paddle)
        else if (ball.dy < 0 && ball.y <= 3) {
            if (ball.x >= paddle2.x && ball.x <= paddle2.x + paddle2.width) {
                score2++;
                ball.dy = -ball.dy;
            } else {
                score2--;
                reset_ball();
            }
        }
        
        send_game_state(0); // msg_type = 0 for ball update
        
        usleep(80000);
    }
    return NULL;
}

void update_paddle(int ch) {
    // Update local paddle position and send update with timestamp
    if (player_role == 0) {
        if (ch == KEY_LEFT && paddle1.x > 2) paddle1.x--;
        if (ch == KEY_RIGHT && paddle1.x < WIDTH - paddle1.width - 2) paddle1.x++;
    } else {
        if (ch == KEY_LEFT && paddle2.x > 2) paddle2.x--;
        if (ch == KEY_RIGHT && paddle2.x < WIDTH - paddle2.width - 2) paddle2.x++;
    }
    // Send update indicating a paddle movement (msg_type = 1)
    send_game_state(1);
}

void reset_ball() {
    ball.x = WIDTH / 2;
    ball.y = HEIGHT / 2;
    ball.dx = 1;
    ball.dy = 1;
}

void *network_handler(void *args) {
    while (game_running) {
        receive_game_state();
        draw(stdscr);
    }
    return NULL;
}

void send_game_state(int msg_type) {
    GameState gs;
    gs.msg_type = msg_type;
    gs.ball_x = ball.x;
    gs.ball_y = ball.y;
    gs.paddle1_x = paddle1.x;
    gs.paddle2_x = paddle2.x;
    gs.game_running = game_running;
    gs.score1 = score1;
    gs.score2 = score2;
    gs.timestamp = get_current_time_ms();
    send(sock, &gs, sizeof(gs), 0);
}

void receive_game_state() {
    GameState gs;
    int bytes = recv(sock, &gs, sizeof(gs), 0);
    if (bytes <= 0) {
        game_running = 0;
        return;
    }
    // Update game state based on received data
    ball.x = gs.ball_x;
    ball.y = gs.ball_y;
    paddle1.x = gs.paddle1_x;
    paddle2.x = gs.paddle2_x;
    game_running = gs.game_running;
    score1 = gs.score1;
    score2 = gs.score2;
    
    // If this is a paddle movement update, compute and log latency.
    if (gs.msg_type == 1) {
        log_latency(gs.timestamp);
    }
}

void synchronize_time() {
    // A very simple synchronization handshake.
    // Server sends its current time in ms; client adjusts its local clock offset.
    if (player_role == 0) {
        long long server_time = get_current_time_ms();
        send(sock, &server_time, sizeof(server_time), 0);
    } else {
        long long server_time;
        recv(sock, &server_time, sizeof(server_time), 0);
        long long client_time = get_current_time_ms() - sync_offset; // current client time before offset
        // Compute offset so that client's clock aligns with server's clock
        sync_offset = server_time - client_time;
    }
}

void log_latency(long long sent_timestamp) {
    long long current_time = get_current_time_ms();
    long long latency = current_time - sent_timestamp;
    
    FILE *f = fopen("latency_log.txt", "a");
    if (f) {
        // Determine message direction based on role.
        if (player_role == 0) {  // Server receives paddle updates from client.
            fprintf(f, "[%lld] Player 2 → Player 1: %lldms\n", current_time, latency);
        } else {  // Client receives updates from server.
            fprintf(f, "[%lld] Player 1 → Player 2: %lldms\n", current_time, latency);
        }
        fclose(f);
    }
}
