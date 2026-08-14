// auto-generated cubic test netlist: struct_cubic_10_4
module struct_cubic_10_4 ( x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, y0, y1, y2, y3 );
input x0, x1, x2, x3, x4, x5, x6, x7, x8, x9;
output y0, y1, y2, y3;
wire a0, a1, a2, a3, c4, a5, a6, c7, a8, a9, c10, a11, a12, c13;
and (a0, x9, x4);
and (a1, x5, x8);
and (a2, x0, x7);
and (a3, a2, x3);
xor (c4, a0, a3);
buf (y0, c4);
and (a5, x0, x2);
and (a6, a5, x1);
xor (c7, a1, a6);
buf (y1, c7);
and (a8, x5, x7);
and (a9, a8, x3);
xor (c10, a0, a9);
buf (y2, c10);
and (a11, x6, x8);
and (a12, a11, x1);
xor (c13, a1, a12);
buf (y3, c13);
endmodule
