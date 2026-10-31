#ifndef WIDGET_ROUTE_H
#define WIDGET_ROUTE_H

/* J4.0: registers /w/calculator, /w/translate, /w/place and /w/stock. One
   handler per kind because the router matches exact paths; the kind selects a
   function, never a host or a template. */
void register_widget_routes(void);

#endif
