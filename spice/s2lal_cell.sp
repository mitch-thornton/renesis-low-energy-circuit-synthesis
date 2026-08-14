* s2lal_cell.sp -- Renesis --spice-gen deck
* circuit: and2   technology: s2lal   gates: 1
* STRUCTURE is exact: one instance per pass device, mirroring the
* Verilog writer and the energy model's device count.  ENERGY from
* this deck is NOT the tool's figure until you replace the stub
* models below with characterized PDK models.
*
* ---------------------------------------------------------------- STUB
* DEVICE MODELS ARE STUBS.  Level-1 MOSFETs sized so that R_on and the
* per-device capacitance are of the family's order; REPLACE with your
* characterized PDK models before drawing any energy conclusion from
* this deck.  The tool's own energy figures do not come from SPICE.
.model NSTUB NMOS (LEVEL=1 VTO=0.3  KP=200u LAMBDA=0.01 CGSO=0.5n CGDO=0.5n)
.model PSTUB PMOS (LEVEL=1 VTO=-0.3 KP=100u LAMBDA=0.01 CGSO=0.5n CGDO=0.5n)
* --------------------------------------------------------------------

* transmission gate: conducts a<->b when t=1 (dual rail: f = NOT t)
.subckt RNS_TG a b t f
MN a t b 0    NSTUB W=2u  L=0.18u
MP a f b VDDB PSTUB W=4u  L=0.18u
.ends

VPHI0 PHI0 0 PWL(0n 0 10n 1.1 20n 1.1 30n 0 40n 0 50n 1.1 60n 1.1 70n 0)
VPHI1 PHI1 0 PWL(0n 0 5n 0 15n 1.1 25n 1.1 35n 0 45n 0 55n 1.1 65n 1.1 75n 0)
VPHI2 PHI2 0 PWL(0n 0 10n 0 20n 1.1 30n 1.1 40n 0 50n 0 60n 1.1 70n 1.1 80n 0)
VPHI3 PHI3 0 PWL(0n 0 15n 0 25n 1.1 35n 1.1 45n 0 55n 0 65n 1.1 75n 1.1 85n 0)
VPHI4 PHI4 0 PWL(0n 0 20n 0 30n 1.1 40n 1.1 50n 0 60n 0 70n 1.1 80n 1.1 90n 0)
VPHI5 PHI5 0 PWL(0n 0 25n 0 35n 1.1 45n 1.1 55n 0 65n 0 75n 1.1 85n 1.1 95n 0)
VPHI6 PHI6 0 PWL(0n 0 30n 0 40n 1.1 50n 1.1 60n 0 70n 0 80n 1.1 90n 1.1 100n 0)
VPHI7 PHI7 0 PWL(0n 0 35n 0 45n 1.1 55n 1.1 65n 0 75n 0 85n 1.1 95n 1.1 105n 0)
VDDB VDDB 0 1.1   ; pMOS bulk
Va_T a_T 0 0
Va_F a_F 0 1.1
Vb_T b_T 0 1.1
Vb_F b_F 0 0

* ---- node y (phase 0)
X1 PHI0 n_y_p_s_1 a_T a_F RNS_TG
X2 n_y_p_s_1 y_T b_T b_F RNS_TG
X3 PHI0 y_F a_F a_T RNS_TG
X4 PHI0 y_F b_F b_T RNS_TG

RB1 y_T 0 1G
RB2 y_F 0 1G

* primary outputs: y_T/y_F
.tran 0.05n 640n
.control
run
set wr_vecnames
wrdata s2lal_cell_po.txt v(y_T) v(y_F)
quit
.endc
.end
