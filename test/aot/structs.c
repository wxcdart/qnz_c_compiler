// AOT Test: Structs
// Tests: Struct definition, member access, struct return

typedef struct {
    int x;
    int y;
} Point;

typedef struct {
    Point origin;
    Point dest;
} Line;

int get_x(Point p) {
    return p.x;
}

Point create_point(int x, int y) {
    Point p;
    p.x = x;
    p.y = y;
    return p;
}

int distance_squared(Point a, Point b) {
    int dx = b.x - a.x;
    int dy = b.y - a.y;
    return dx * dx + dy * dy;
}

int main() {
    Point p1 = create_point(0, 0);
    Point p2 = create_point(3, 4);
    
    if (p1.x != 0) return 1;
    if (p1.y != 0) return 2;
    if (p2.x != 3) return 3;
    if (p2.y != 4) return 4;
    
    int dist = distance_squared(p1, p2);
    if (dist != 25) return 5;
    
    return 0;
}
