// ****************************************************************************
// Calculating star coordinates through Barnes–Hut simulation.
// All stars are spread in a square quad-tree.
// https://en.wikipedia.org/wiki/Barnes%E2%80%93Hut_simulation
// ****************************************************************************

#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <GLFW/glfw3.h>
#include "common.h"
#include "linmath.h"
#include "world.h"

// Star or quadrant
struct node
{                  // inherits vecd2
    struct vecd2;  // center of mass
    double mass;
    double size;  // zero for a star
};

static struct star
{
    struct node;  // inherits struct node
    struct vecd2 speed;
    struct vecd2 accel;  // already multiplied by t/2, for better performance
} *stars = NULL;

static struct quad
{
    struct node;  // inherits struct node
    struct vecd2 center; // geometrical center
    struct quad* children[4];  // 4 quadrants
} *quads = NULL;

static int cores;
static pthread_t *threads = NULL;  // thread pool
static sem_t job_start;  // thread pool semaphores
static sem_t job_finish;
static double frame_time;  // stays constant during a frame

void finalize_world()
{
    if (threads) {
        for (int i = 0; i < cores; i++)
            pthread_cancel(threads[i]);
        for (int i = 0; i < cores; i++)
            pthread_join(threads[i], NULL);
        sem_destroy(&job_start);
        sem_destroy(&job_finish);
        free(threads);
        threads = NULL;
    }
    if (stars) {
        free(stars);
        stars = NULL;
    }
    if (quads) {
        free(quads);
        quads = NULL;
    }
    if (disp_stars) {
        free(disp_stars);
        disp_stars = NULL;
    }
}

// Recursive walk through the qtree
static void get_accel(struct star* star, const struct quad* node, struct vecd2* accel)
{
    double dx = node->x - star->x;
    double dy = node->y - star->y;
    double distance_sqr = dx*dx + dy*dy;
    if (sqrt(distance_sqr) > node->size * config.accuracy) {
        double angle = atan2(dy, dx);
        double accel_abs = node->mass / (distance_sqr + config.epsilon);
        accel->x += accel_abs * cos(angle);
        accel->y += accel_abs * sin(angle);
    } else if (node->size) {
        if (node->children[0])
            get_accel(star, node->children[0], accel);
        if (node->children[1])
            get_accel(star, node->children[1], accel);
        if (node->children[2])
            get_accel(star, node->children[2], accel);
        if (node->children[3])
            get_accel(star, node->children[3], accel);
    } // else the same star or another star with the same coordinates
}

// Sleeps in the pool until job_start is fired.
// Can be safely cancelled at any time.
static void* update_stars_job(void* arg)
{
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    int part = (int)(intptr_t)arg;
    int part_start = config.stars * part / cores;
    int part_end = config.stars * (part+1) / cores;

    while (true) {
        sem_wait(&job_start);
        for (int i = part_start; i < part_end; i++) {
            struct vecd2 accel = { 0 };
            get_accel(&stars[i], &quads[0], &accel);
            accel.x *= frame_time * config.gravity / 2;
            accel.y *= frame_time * config.gravity / 2;
            stars[i].speed.x += stars[i].accel.x + accel.x;  // velocity Verlet integration
            stars[i].speed.y += stars[i].accel.y + accel.y;
            stars[i].accel = accel;
        }
        sem_post(&job_finish);
    }

    return NULL;
}

static inline double frand(double min, double max)
{
    return (double)rand()/RAND_MAX * (max-min) + min;
}

// assists qsorting
static int mass_ascending(const void *a, const void *b)
{
    if (((struct star*)a)->mass < ((struct star*)b)->mass) return -1;
    if (((struct star*)a)->mass > ((struct star*)b)->mass) return 1;
    return 0;
}

void init_world()
{
    assert(config.stars > 1);

    // Init threads
    cores = sysconf(_SC_NPROCESSORS_ONLN);
    #if 0
        #warning single-threaded
        cores = 1;
    #endif
    if (cores > 1) {
        sem_init(&job_start, 0, 0);
        sem_init(&job_finish, 0, 0);
        threads = malloc(cores * sizeof(pthread_t));
        for (int i = 0; i < cores; i++)
            pthread_create(&threads[i], NULL, &update_stars_job, (void*)(intptr_t)i);
    }

    // Init stars
    stars = calloc(config.stars, sizeof(struct star));
    quads = calloc(2 * config.stars, sizeof(struct quad));  // TODO: dynamic reallocation
    disp_stars = malloc(config.stars * sizeof(vec2));
    double rmax = sqrt(config.stars) / config.galaxy_density;
    for (int i = 0; i < config.stars; i++) {
        double r = frand(0, rmax);
        double dir = frand(0, 2*M_PI);
        stars[i].x = r * cos(dir);
        stars[i].y = r * sin(dir);
        stars[i].speed.x =  config.star_speed * pow(r, 0.25) * sin(dir);
        stars[i].speed.y = -config.star_speed * pow(r, 0.25) * cos(dir);
        stars[i].mass = frand(1, 10);
    }
    qsort(stars, config.stars, sizeof(struct star), mass_ascending);  // increases accumulation accuracy

    #if 0
        config.stars = 3;
        stars[0].x = 0.05;
        stars[0].y = 0;
        stars[0].speed.x = 0;
        stars[0].speed.y = -0.0;
        stars[0].mass = 1;
        stars[1].x = -0.05;
        stars[1].y = 0;
        stars[1].speed.x = 0;
        stars[1].speed.y = 0.0;
        stars[1].mass = 1;
        stars[2].x = 1000;
        stars[2].y = 1000;
    #endif
}

// 2 3
// 0 1
static inline int get_quadrant(const struct quad *quad, const struct star *star)
{
    int quadrant = 0;
    if (star->x > quad->center.x)
        quadrant++;
    if (star->y > quad->center.y)
        quadrant += 2;
    return quadrant;
}

void world_frame(double time)
{
    frame_time = time;
    if (frame_time > 1/config.min_fps)
        frame_time = 1/config.min_fps;
    frame_time *= config.speed;


    //************************
    // Build Barnes-Hut qtree
    //************************

    perf_build = glfwGetTime();
    // Root node
    double xmin_world = INFINITY;
    double ymin_world = INFINITY;
    double xmax_world = -INFINITY;
    double ymax_world = -INFINITY;
    for (int i = 0; i < config.stars; i++) {
        if (xmin_world > stars[i].x)
            xmin_world = stars[i].x;
        if (xmax_world < stars[i].x)
            xmax_world = stars[i].x;
        if (ymin_world > stars[i].y)
            ymin_world = stars[i].y;
        if (ymax_world < stars[i].y)
            ymax_world = stars[i].y;
    }
    quads[0].center.x = (xmin_world+xmax_world)/2;
    quads[0].center.y = (ymin_world+ymax_world)/2;
    double size_x = xmax_world - xmin_world;
    double size_y = ymax_world - ymin_world;
    quads[0].size = size_x > size_y ? size_x : size_y;  // keep nodes square
    size_t quad_count = 1;  // number of quads

    // Build the tree
    for (struct star* star = stars; star < stars + config.stars; star++) {
        struct quad* quad = &quads[0];
        do {
            // Add star to current quad
            double mass_sum = quad->mass + star->mass;
            quad->x = (quad->x * quad->mass + star->x * star->mass) / mass_sum;
            quad->y = (quad->y * quad->mass + star->y * star->mass) / mass_sum;
            quad->mass = mass_sum;
            int quadrant = get_quadrant(quad, star);
            if (quad->children[quadrant] == NULL) {
                quad->children[quadrant] = (struct quad*)star;
            } else if (quad->children[quadrant]->size == 0) {
                struct star* old_star = (struct star*)(quad->children[quadrant]);
                struct quad* new_quad = &quads[quad_count];
                quad_count++;
                new_quad->x = old_star->x;
                new_quad->y = old_star->y;
                new_quad->mass = old_star->mass;
                new_quad->size = quad->size/2;
                double shift = quad->size/4;
                new_quad->center.x = quad->center.x + (quadrant&0x1 ? shift : -shift);
                new_quad->center.y = quad->center.y + (quadrant&0x2 ? shift : -shift);
                new_quad->children[get_quadrant(new_quad, old_star)] = (struct quad*)old_star;
                quad->children[quadrant] = new_quad;
            }
            quad = quad->children[quadrant];
        } while (quad->size);
    }
    perf_build = glfwGetTime() - perf_build;


    //*************************************
    // Calculate acceleration and position
    //*************************************

    perf_accel = glfwGetTime();
    // Wake up the threads in the pool
    if (cores > 1) {
        for (int i = 0; i < cores; i++)
            sem_post(&job_start);
        for (int i = 0; i < cores; i++)
            sem_wait(&job_finish);
    } else {
        for (int i = 0; i < config.stars; i++) {
            struct vecd2 accel = { 0 };
            get_accel(&stars[i], &quads[0], &accel);
            accel.x *= frame_time * config.gravity / 2;
            accel.y *= frame_time * config.gravity / 2;
            stars[i].speed.x += stars[i].accel.x + accel.x;  // velocity Verlet integration
            stars[i].speed.y += stars[i].accel.y + accel.y;
            stars[i].accel = accel;
        }
    }
    for (int i = 0; i < config.stars; i++) {
        stars[i].x += frame_time * (stars[i].speed.x + stars[i].accel.x);  // velocity Verlet integration
        stars[i].y += frame_time * (stars[i].speed.y + stars[i].accel.y);
    }
    perf_accel = glfwGetTime() - perf_accel;

    // Display coordinates in GLfloat[]
    for (int i = 0; i < config.stars; i++) {
        disp_stars[i][0] = stars[i].x;
        disp_stars[i][1] = stars[i].y;
    }
    memset(quads, 0, quad_count * sizeof(struct quad));
}
