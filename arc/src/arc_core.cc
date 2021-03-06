
/*********************************************************************************
 *  ARC-SRTK - Single Frequency RTK Pisitioning Library
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  Created on: July 07, 2017
 *      Author: SuJingLan
 *********************************************************************************/

/**
 * @file arc_core.cc
 * @brief Source file for the ARC-SRTK Library
 * @author SuJingLan
 */

#include "arc.h"
#include "libPF/ParticleFilter.h"
#include "arc_ObservationModel.h"
#include "arc_MovementModel.h"

#include <iomanip>
#include <fstream>

using namespace std;
/* constants/global variables ------------------------------------------------*/
#define SQRT(x)     ((x)<=0.0?0.0:sqrt(x))

#define MAXINFILE   1000        /* max number of input files */
#define USEPNTINI   1           /* using the standard position to inital solution*/
#define USERTKLIB   0           /* using rtk position function of RTKLIB */

static pcvs_t pcvss={0};        /* receiver antenna parameters */
static pcvs_t pcvsr={0};        /* satellite antenna parameters */
static obs_t obss={0};          /* observation data */
static nav_t navs={0};          /* navigation data */

static sta_t stas[MAXRCV];      /* station infomation */
static int nepoch=0;            /* number of observation epochs */
static int iobsu =0;            /* current rover observation data index */
static int iobsr =0;            /* current reference observation data index */
static int isbs  =0;            /* current sbas message index */
static int revs  =0;            /* analysis direction (0:forward,1:backward) */
static int aborts=0;            /* abort status */
static sol_t *solf;             /* forward solutions */
static sol_t *solb;             /* backward solutions */
static double *rbf;             /* forward base positions */
static double *rbb;             /* backward base positions */
static int isolf=0;             /* current forward solutions index */
static int isolb=0;             /* current backward solutions index */
static char proc_rov [64]="";   /* rover for current processing */
static char proc_base[64]="";   /* base station for current processing */
typedef libPF::ParticleFilter<ARC::ARC_States> ParticleFilterType;
/* particle filter type */

/* number of parameters (pos,ionos,tropos,hw-bias,phase-bias,real,estimated) */
#define NF(opt)     ((opt)->ionoopt==IONOOPT_IFLC?1:(opt)->nf)
#define NP(opt)     ((opt)->dynamics==0?3:6)
#define NPDC(opt)   ((opt)->dynamics_dc==0?3:6)
#define NCLK(opt)   ((opt)->est_doppler==0?0:1)
#define NI(opt)     ((opt)->ionoopt!=IONOOPT_EST?0:MAXSAT)
#define NT(opt)     ((opt)->tropopt<TROPOPT_EST?0:((opt)->tropopt<TROPOPT_ESTG?2:6))
#define NL(opt)     ((opt)->glomodear!=2?0:NFREQGLO)
#define NB(opt)     ((opt)->mode<=PMODE_DGPS?0:MAXSAT*NF(opt))
#define NR(opt)     (NP(opt)+NI(opt)+NT(opt)+NL(opt)+NCLK(opt))
#define NX(opt)     (NR(opt)+NB(opt))
#define NXDC(opt)   (NPDC(opt)+NCLK(opt))

/* state variable index */
#define II(s,opt)   (NP(opt)+NCLK(opt)+(s)-1)       /* ionos (s:satellite no) */
#define IT(r,opt)   (NP(opt)+NCLK(opt)+NI(opt)+NT(opt)/2*(r))
#define IL(f,opt)   (NP(opt)+NI(opt)+NT(opt)+(f))   /* receiver h/w bias */
/* tropos (r:0=rov,1:ref) */
#define IC(opt)     (NP(opt))                       /* rover station clock drift */
#define ICDC(opt)   (NPDC(opt))                     /* rover station clock drift */
#define IB(s,f,opt) (NR(opt)+MAXSAT*(f)+(s)-1)      /* phase bias (s:satno,f:freq) */

/* show message and check break ----------------------------------------------*/
static int arc_checkbrk(const char *format, ...)
{
    va_list arg;
    char buff[1024],*p=buff;
    if (!*format) return arc_showmsg("");
    va_start(arg,format);
    p+=vsprintf(p,format,arg);
    va_end(arg);
    if (*proc_rov&&*proc_base) sprintf(p," (%s-%s)",proc_rov,proc_base);
    else if (*proc_rov ) sprintf(p," (%s)",proc_rov );
    else if (*proc_base) sprintf(p," (%s)",proc_base);
    return arc_showmsg(buff);
}
/* search next observation data index ----------------------------------------*/
static int arc_nextobsf(const obs_t *obs, int *i, int rcv)
{
    double tt;
    int n;

    if (*i<0) {
        fprintf(stderr,"ARC-SRTK: It have happened some unkowned problems \n");
        return 0;
    }
    for (;*i<obs->n;(*i)++) if (obs->data[*i].rcv==rcv) break;

    for (n=0;*i+n<obs->n;n++) {
        tt=timediff(obs->data[*i+n].time,obs->data[*i].time);
        if (obs->data[*i+n].rcv!=rcv||tt>DTTOL) break;
    }
    return n;
}
static int arc_nextobsb(const obs_t *obs, int *i, int rcv)
{
    double tt;
    int n;

    if (*i<0) {
        fprintf(stderr,"ARC-SRTK: It have happened some unkowned problems \n");
        return 0;
    }
    for (;*i>=0;(*i)--) if (obs->data[*i].rcv==rcv) break;
    for (n=0;*i-n>=0;n++) {
        tt=timediff(obs->data[*i-n].time,obs->data[*i].time);
        if (obs->data[*i-n].rcv!=rcv||tt<-DTTOL) break;
    }
    return n;
}
/* input obs data, navigation messages and sbas correction -------------------*/
static int arc_inputobs(obsd_t *obs, int solq, const prcopt_t *popt,int *nu,int *nr)
{
    gtime_t time={0};
    int i,n=0;

    arc_log(ARC_INFO,"infunc  : revs=%d iobsu=%d iobsr=%d isbs=%d\n",revs,iobsu,iobsr,isbs);

    if (0<=iobsu&&iobsu<obss.n) {
        arc_settime((time=obss.data[iobsu].time));
        if (arc_checkbrk("processing : %s Q=%d",time_str(time,0),solq)) {
            aborts=1;
            arc_showmsg("aborted"); return -1;
        }
    }
    if (!revs) { /* input forward data */
        if ((*nu=arc_nextobsf(&obss,&iobsu,1))<=0) return -1;
        if (popt->intpref) {
            for (;(*nr=arc_nextobsf(&obss,&iobsr,2))>0;iobsr+=*nr)
                if (timediff(obss.data[iobsr].time,obss.data[iobsu].time)>-DTTOL) break;
        }
        else {
            for (i=iobsr;(*nr=arc_nextobsf(&obss,&i,2))>0;iobsr=i,i+=*nr)
                if (timediff(obss.data[i].time,obss.data[iobsu].time)>DTTOL) break;
        }
        *nr=arc_nextobsf(&obss,&iobsr,2);
        for (i=0;i<*nu&&n<MAXOBS*2;i++) obs[n++]=obss.data[iobsu+i];
        for (i=0;i<*nr&&n<MAXOBS*2;i++) obs[n++]=obss.data[iobsr+i];
        iobsu+=*nu;
    }
    else { /* input backward data */
        if ((*nu=arc_nextobsb(&obss,&iobsu,1))<=0) return -1;
        if (popt->intpref) {
            for (;(*nr=arc_nextobsb(&obss,&iobsr,2))>0;iobsr-=*nr)
                if (timediff(obss.data[iobsr].time,obss.data[iobsu].time)<DTTOL) break;
        }
        else {
            for (i=iobsr;(*nr=arc_nextobsb(&obss,&i,2))>0;iobsr=i,i-=*nr)
                if (timediff(obss.data[i].time,obss.data[iobsu].time)<-DTTOL) break;
        }
        *nr=arc_nextobsb(&obss,&iobsr,2);
        for (i=0;i<*nu&&n<MAXOBS*2;i++) obs[n++]=obss.data[iobsu-*nu+1+i];
        for (i=0;i<*nr&&n<MAXOBS*2;i++) obs[n++]=obss.data[iobsr-*nr+1+i];
        iobsu-=*nu;
    }
    return n;
}
/* select common satellites between rover and reference station --------------*/
static int arc_selcomsat(const obsd_t *obs, const rtk_t* rtk,int nu, int nr,
                         const prcopt_t *opt, int *sat, int *iu, int *ir)
{
    int i,j,k=0;

    arc_log(ARC_INFO, "arc_selsat  : nu=%d nr=%d\n", nu, nr);

    for (i=0,j=nu;i<nu&&j<nu+nr;i++,j++) {
        if      (obs[i].sat<obs[j].sat) j--;
        else if (obs[i].sat>obs[j].sat) i--;
        else if (rtk->ssat[obs[j].sat-1].azel[1]>=opt->elmin) {
            sat[k]=obs[i].sat; iu[k]=i; ir[k++]=j;
            arc_log(ARC_INFO,"(%2d) sat=%3d iu=%2d ir=%2d\n",k-1,obs[i].sat,i,j);
        }
    }
    return k;
}
/* carrier-phase bias correction by fcb --------------------------------------*/
static void arc_corr_phase_bias_fcb(obsd_t *obs,int n,const nav_t *nav)
{
    int i,j,k;

    for (i=0;i<nav->nf;i++) {
        if (timediff(nav->fcb[i].te,obs[0].time)<-1E-3) continue;
        if (timediff(nav->fcb[i].ts,obs[0].time)> 1E-3) break;
        for (j=0;j<n;j++) {
            for (k=0;k<NFREQ;k++) {
                if (obs[j].L[k]==0.0) continue;
                obs[j].L[k]-=nav->fcb[i].bias[obs[j].sat-1][k];
            }
        }
        return;
    }
}
#if 1
/* particle process positioning ------------------------------------------------*/
static void arc_procpos_pf(const prcopt_t *popt, const solopt_t *sopt,
                           int mode)
{
    rtk_t rtk;
    obsd_t obs[MAXOBS*2];
    gtime_t time={0};
    int i,nobs,n,ns,rsat[MAXSAT],usat[MAXSAT],nu=0,nr=0,sat[MAXSAT],first=1;
    char msg[126];

    arc_log(ARC_INFO,"arc_procpos : mode=%d\n",mode);

    /* rtk struct date type initial */
    arc_rtkinit(&rtk, popt);

    /* arc-srtk observation model and states movement model  */
    ARC::ARC_ObservationModel ObsModel;
    ARC::ARC_MovementModel    MoveModel(popt,&rtk);
    ARC::ARC_States           States(popt);
    ParticleFilterType        PF(ARC_PF_NUM,&ObsModel,&MoveModel);
    /* set the observation settings and solution data */
    ObsModel.SetOpt(popt);
    ObsModel.SetNav(&navs);
    ObsModel.SetSRTK(&rtk);
    ObsModel.SetDDorSD(ARC_PF_USE_DD);
    MoveModel.SetNav(&navs);
    MoveModel.SetSRTK(&rtk);
    /* set the particle filter resample */
    PF.setResamplingMode(libPF::RESAMPLE_ALWAYS);

    while ((nobs=arc_inputobs(obs,rtk.sol.stat,popt,&nu,&nr))>=0) {
        /*abort */
        if (aborts) break;

        /* precious epoch time */
        time=rtk.sol.time;

        /* exclude satellites */
        for (i=n=0;i<nobs;i++) {
            if ((satsys(obs[i].sat,NULL)&popt->navsys)&&
                popt->exsats[obs[i].sat-1]!=1) obs[n++]=obs[i];
        }
        if (n<=0) continue;
        /* count rover/base station observations */
        for (nu=0;nu   <n&&obs[nu   ].rcv==1;nu++) ;   /* rover */
        for (nr=0;nu+nr<n&&obs[nu+nr].rcv==2;nr++) ;   /* base */

#if USEPNTINI
        /* rover position by single point positioning */
        if (!arc_pntpos(obs,nu,&navs,&rtk.opt,&rtk.sol,NULL,rtk.ssat,msg)) {
            arc_log(ARC_WARNING,"arc-srtk point pos error (%s)\n",msg);
            continue;
        }
        /* inital base station and rover station observation time difference */
        if (time.time!=0) rtk.tt=timediff(rtk.sol.time,time);

        /* to integrate a known prior state use */
        if (first) {
            for (i=0;i<6;i++) States.SetStatesValue(rtk.sol.rr[i],i);
            PF.setPriorState(States);
            first=0;
        }
#endif
        /* select common satellites between rover and reference station */
        if ((ns=arc_selcomsat(obs,&rtk,nu,nr,popt,sat,usat,rsat))<=0) continue;

        for (i=0;i<ARC_PF_ITERS;i++) {
            if (i==0) {
                /* asign common satellites to movement model */
                MoveModel.SetComSatList(rsat,usat,sat,ns);
                /* set gps and bds observation of states movement */
                MoveModel.SetObs(obs,nu+nr);
                /* states movement model for predicting the states */
                MoveModel.drift(States,rtk.tt);
                /* set the observations of gps and bds */
                ObsModel.SetObs(obs,nr+nu);
            } else {
                MoveModel.SetRoverStd(MoveModel.getRoverStd()/2.0);
                MoveModel.SetAmbStd(MoveModel.getAmbMin()/2.0,MoveModel.getAmbMax()/2.0);
            }
            /* set the states of observation model */
            ObsModel.setStates(States);

            /* particle filter */
            PF.filter();
            /* save particle result to rtk_t type struct */
            States=PF.getMmseEstimate();
        }
        MoveModel.SetRoverStd(ARC_PF_ROVERPOS_STD);
        MoveModel.SetAmbStd(ARC_PF_AMB_MIN,ARC_PF_AMB_MAX);

        LOG(INFO)<<"Particle Filter is Running ... : "
                 <<setiosflags(ios::fixed)<<setprecision(6)
                 <<PF.getMmseEstimate().getStateValue(0)<<"   "
                 <<PF.getMmseEstimate().getStateValue(1)<<"   "
                 <<PF.getMmseEstimate().getStateValue(2);
    }
    arc_rtkfree(&rtk);
}
#endif
/* process positioning -------------------------------------------------------*/
FILE *fp=fopen("/home/sujinglan/arc_rtk/arc_test/result/gps-pos","w");
static void arc_procpos(FILE* fp,const prcopt_t *popt, const solopt_t *sopt,
                        int mode)
{
    static const int count=20;
    gtime_t time={0};
    sol_t sol={{0}};
    rtk_t rtk;
    obsd_t obs[MAXOBS*2]; /* for rover and base */
    double rb[3]={0},dt,pgBar,pos[3];
    int i,nobs,n,pri[]={0,1,2,3,4,5,1,6},nu=0,nr=0,c=0,solstatic;
    char prn[8]={0},str[128]="",time_str_[126]="",stats[16]="";

    arc_log(ARC_INFO,"arc_procpos : mode=%d\n",mode);
    arc_info(15,4,"relative position start");

    solstatic=sopt->solstatic&&(popt->mode==PMODE_STATIC);

#if USERTKLIB
    rtkinit(&rtk,popt);
#else
    arc_rtkinit(&rtk,popt);
#endif

    while ((nobs=arc_inputobs(obs,rtk.sol.stat,popt,&nu,&nr))>=0) {
        /*abort */
        if (aborts) break;

        /* exclude satellites */
        for (i=n=0;i<nobs;i++) {
            if ((satsys(obs[i].sat,NULL)&popt->navsys)&&
                popt->exsats[obs[i].sat-1]!=1) obs[n++]=obs[i];
            if (satsys(obs[i].sat,NULL)==SYS_CMP
                &&popt->exsats[obs[i].sat-1]==1) {  /* just for debug */
                satno2id(obs[i].sat,prn);
                arc_log(ARC_WARNING,"arc_procpos : "
                        "excluded bds geo satellite %s,sat no is %d",prn,obs[i].sat);
            }
        }
        if (n<=0) continue; /* no observations */

        /* carrier-phase bias correction */
        if (navs.nf>0) {
            arc_corr_phase_bias_fcb(obs,n,&navs);
        }

#if USERTKLIB
        if (!arc_srtkpos2(&rtk,obs,n,&navs)) continue;
#else
        arc_srtkpos(&rtk,obs,n,&navs);
#endif

        /* previous epoch */
        time=rtk.sol.time;

        /* calculate the time that has elapsed */
        if (time.time) {
            dt=fabs(timediff(time,popt->ts));
            pgBar=dt/fabs(timediff(popt->te,popt->ts))*(85.0)+15.0;
            if (c++>=count) {
                strcpy(time_str_,time_str(time,2));
                strcpy(stats,rtk.sol.stat==SOLQ_FIX?": FIX":
                             rtk.sol.stat==SOLQ_FLOAT?": FLOAT":
                             rtk.sol.stat==SOLQ_DGPS?": DGPS":
                             rtk.sol.stat==SOLQ_SINGLE?": SINGLE":
                             rtk.sol.stat==SOLQ_DR?": DR":
                             rtk.sol.stat==SOLQ_HALFFIX?": HALFFIX":
                             rtk.sol.stat==SOLQ_INHERITFIX?": INHERIXFIX":": NONE");
                sprintf(str,"%s%s",time_str_,stats);
                arc_info(int(pgBar),3,str);
                c=0; /* reset log-informations counts */
            }
        }
        if (mode==0) { /* forward/backward */
            if (!solstatic) {
                outsol(fp,&rtk.sol,rtk.rb,sopt);
            }
            else if (time.time==0||pri[rtk.sol.stat]<=pri[sol.stat]) {
                sol=rtk.sol;
                for (i=0;i<3;i++) rb[i]=rtk.rb[i];
                if (time.time==0||timediff(rtk.sol.time,time)<0.0) {
                    time=rtk.sol.time;
                }
            }
        }
        else if (!revs) { /* combined-forward */
            if (isolf>=nepoch) return;
            solf[isolf]=rtk.sol;
            for (i=0;i<3;i++) rbf[i+isolf*3]=rtk.rb[i];
            isolf++;
        }
        else { /* combined-backward */
            if (isolb>=nepoch) return;
            solb[isolb]=rtk.sol;
            for (i=0;i<3;i++) rbb[i+isolb*3]=rtk.rb[i];
            isolb++;
        }
    }
    if (mode==0&&solstatic&&time.time!=0.0) {
        sol.time=time;
        outsol(fp,&sol,rb,sopt);
    }
#if USERTKLIB
    rtkfree(&rtk);
#else
    arc_rtkfree(&rtk);
#endif
    arc_info(100,4,"relative position is end,now ploting results...");
}
/* validation of combined solutions ------------------------------------------*/
static int arc_valcomb(const sol_t *solf, const sol_t *solb)
{
    double dr[3],var[3];
    int i;
    char tstr[32];

    arc_log(ARC_INFO,"arc_valcomb :\n");

    /* compare forward and backward solution */
    for (i=0;i<3;i++) {
        dr[i]=solf->rr[i]-solb->rr[i];
        var[i]=solf->qr[i]+solb->qr[i];
    }
    for (i=0;i<3;i++) {
        if (dr[i]*dr[i]<=16.0*var[i]) continue; /* ok if in 4-sigma */

        time2str(solf->time,tstr,2);
        arc_log(ARC_INFO,"degrade fix to float: %s dr=%.3f %.3f %.3f std=%.3f %.3f %.3f\n",
                tstr+11,dr[0],dr[1],dr[2],SQRT(var[0]),SQRT(var[1]),SQRT(var[2]));
        return 0;
    }
    return 1;
}
/* combine forward/backward solutions and output results ---------------------*/
static void arc_combres(FILE *fp,const prcopt_t *popt, const solopt_t *sopt)
{
    gtime_t time={0};
    sol_t sols={{0}},sol={{0}};
    double tt,Qf[9],Qb[9],Qs[9],rbs[3]={0},rb[3]={0},rr_f[3],rr_b[3],rr_s[3];
    int i,j,k,solstatic,pri[]={0,1,2,3,4,5,1,6};

    arc_log(ARC_INFO,"combres : isolf=%d isolb=%d\n",isolf,isolb);

    solstatic=sopt->solstatic&&
              (popt->mode==PMODE_STATIC||popt->mode==PMODE_PPP_STATIC);

    for (i=0,j=isolb-1;i<isolf&&j>=0;i++,j--) {

        if ((tt=timediff(solf[i].time,solb[j].time))<-DTTOL) {
            sols=solf[i];
            for (k=0;k<3;k++) rbs[k]=rbf[k+i*3];
            j++;
        }
        else if (tt>DTTOL) {
            sols=solb[j];
            for (k=0;k<3;k++) rbs[k]=rbb[k+j*3];
            i--;
        }
        else if (solf[i].stat<solb[j].stat) {
            sols=solf[i];
            for (k=0;k<3;k++) rbs[k]=rbf[k+i*3];
        }
        else if (solf[i].stat>solb[j].stat) {
            sols=solb[j];
            for (k=0;k<3;k++) rbs[k]=rbb[k+j*3];
        }
        else {
            sols=solf[i];
            sols.time=timeadd(sols.time,-tt/2.0);

            if ((popt->mode==PMODE_KINEMA||popt->mode==PMODE_MOVEB)&&
                sols.stat==SOLQ_FIX) {

                /* degrade fix to float if validation failed */
                if (!arc_valcomb(solf+i,solb+j)) sols.stat=SOLQ_FLOAT;
            }
            for (k=0;k<3;k++) {
                Qf[k+k*3]=solf[i].qr[k];
                Qb[k+k*3]=solb[j].qr[k];
            }
            Qf[1]=Qf[3]=solf[i].qr[3];
            Qf[5]=Qf[7]=solf[i].qr[4];
            Qf[2]=Qf[6]=solf[i].qr[5];
            Qb[1]=Qb[3]=solb[j].qr[3];
            Qb[5]=Qb[7]=solb[j].qr[4];
            Qb[2]=Qb[6]=solb[j].qr[5];

            if (popt->mode==PMODE_MOVEB) {
                for (k=0;k<3;k++) rr_f[k]=solf[i].rr[k]-rbf[k+i*3];
                for (k=0;k<3;k++) rr_b[k]=solb[j].rr[k]-rbb[k+j*3];
                if (arc_smoother(rr_f,Qf,rr_b,Qb,3,rr_s,Qs)) continue;
                for (k=0;k<3;k++) sols.rr[k]=rbs[k]+rr_s[k];
            }
            else {
                if (arc_smoother(solf[i].rr,Qf,solb[j].rr,Qb,3,sols.rr,Qs)) continue;
            }
            sols.qr[0]=(float)Qs[0];
            sols.qr[1]=(float)Qs[4];
            sols.qr[2]=(float)Qs[8];
            sols.qr[3]=(float)Qs[1];
            sols.qr[4]=(float)Qs[5];
            sols.qr[5]=(float)Qs[2];
        }
        if (!solstatic) {
            outsol(fp,&sols,rbs,sopt);
        }
        else if (time.time==0||pri[sols.stat]<=pri[sol.stat]) {
            sol=sols;
            for (k=0;k<3;k++) rb[k]=rbs[k];
            if (time.time==0||timediff(sols.time,time)<0.0) {
                time=sols.time;
            }
        }
    }
    if (solstatic&&time.time!=0.0) {
        sol.time=time;
        outsol(fp,&sol,rb,sopt);
    }
}
/* read prec ephemeris, sbas data, lex data, tec grid and open rtcm ----------*/
static void arc_readpreceph(char **infile, int n, const prcopt_t *prcopt,
                            nav_t *nav)
{
    seph_t seph0={0};
    int i;
    char *ext;

    arc_log(ARC_INFO,"arc_readpreceph: n=%d\n",n);

    nav->ne=nav->nemax=0;
    nav->nc=nav->ncmax=0;
    nav->nf=nav->nfmax=0;

    /* read precise ephemeris files */
    for (i=0;i<n;i++) {
        if (strstr(infile[i],"%r")||strstr(infile[i],"%b")) continue;
        arc_readsp3(infile[i],nav,0);
    }
    /* read precise clock files */
    for (i=0;i<n;i++) {
        if (strstr(infile[i],"%r")||strstr(infile[i],"%b")) continue;
        arc_readrnxc(infile[i],nav);
    }
    /* read satellite fcb files */
    for (i=0;i<n;i++) {
        if (strstr(infile[i],"%r")||strstr(infile[i],"%b")) continue;
        if ((ext=strrchr(infile[i],'.'))&&
            (!strcmp(ext,".fcb")||!strcmp(ext,".FCB"))) {
            arc_readfcb(infile[i], nav);
        }
    }
    /* allocate sbas ephemeris */
    nav->ns=nav->nsmax=NSATSBS*2;
    if (!(nav->seph=(seph_t *)malloc(sizeof(seph_t)*nav->ns))) {
        arc_log(ARC_ERROR, "error : sbas ephem memory allocation");
        return;
    }
    for (i=0;i<nav->ns;i++) nav->seph[i]=seph0;
}
/* free prec ephemeris and sbas data -----------------------------------------*/
static void arc_freepreceph(nav_t *nav)
{
    int i;

    arc_log(ARC_INFO, "arc_freepreceph:\n");

    free(nav->peph); nav->peph=NULL; nav->ne=nav->nemax=0;
    free(nav->pclk); nav->pclk=NULL; nav->nc=nav->ncmax=0;
    free(nav->fcb ); nav->fcb =NULL; nav->nf=nav->nfmax=0;
    free(nav->seph); nav->seph=NULL; nav->ns=nav->nsmax=0;
}
/* read obs and nav data -----------------------------------------------------*/
static int arc_readobsnav(gtime_t ts, gtime_t te, double ti, char **infile,
                          const int *index, int n, prcopt_t *prcopt,
                          obs_t *obs, nav_t *nav, sta_t *sta)
{
    int i,ind=0,nobs=0,rcv=1;

    arc_log(ARC_INFO,"arc_readobsnav: ts=%s n=%d\n",time_str(ts, 0), n);

    obs->data=NULL; obs->n =obs->nmax =0;
    nav->eph =NULL; nav->n =nav->nmax =0;
    nav->geph=NULL; nav->ng=nav->ngmax=0;
    nepoch=0;

    for (i=0;i<n;i++) {
        if (arc_checkbrk("")) return 0;

        if (index[i]!=ind) {
            if (obs->n>nobs) rcv++;
            ind=index[i]; nobs=obs->n;
        }
        /* read rinex obs and nav file */
        arc_info(3+i,3,"read rinex obs and nav file");
        if (arc_readrnxt(infile[i],rcv,ts,te,ti,prcopt->rnxopt[rcv<=1?0:1],obs,nav,
                         rcv<=2?sta+rcv-1:NULL)<0) {
            arc_log(ARC_WARNING,"insufficient memory\n");
            arc_info(3+i,2,"insufficient memory");
            return 0;
        }
    }
    if (obs->n<=0) {
        arc_log(ARC_WARNING,"readobsnav : error , no obs data");
        arc_info(3+i,2,"error,no obs data");
        return 0;
    }
    if (nav->n<=0&&nav->ng<=0&&nav->ns<=0) {
        arc_log(ARC_WARNING,"readobsnav : error , no nav data \n");
        arc_info(3+n,2,"error,no nav data");
        return 0;
    }
    /* sort observation data */
    arc_info(3+n+2,3,"sort observation data");
    nepoch=sortobs(obs);

    /* save observation start and end time */
    prcopt->ts=obs->data[0       ].time;
    prcopt->te=obs->data[obs->n-1].time;

    /* delete duplicated ephemeris */
    uniqnav(nav);
    arc_info(3+n+4,3,"read obs and nav data is done");
    return 1;
}
/* free obs and nav data -----------------------------------------------------*/
static void arc_freeobsnav(obs_t *obs, nav_t *nav)
{
    arc_log(ARC_INFO, "arc_freeobsnav:\n");

    free(obs->data); obs->data=NULL; obs->n =obs->nmax =0;
    free(nav->eph ); nav->eph =NULL; nav->n =nav->nmax =0;
    free(nav->geph); nav->geph=NULL; nav->ng=nav->ngmax=0;
    free(nav->seph); nav->seph=NULL; nav->ns=nav->nsmax=0;
}
/* average of single position ------------------------------------------------*/
static int arc_avepos(double *ra, int rcv, const obs_t *obs, const nav_t *nav,
                      const prcopt_t *opt)
{
    obsd_t data[MAXOBS]={0};
    gtime_t ts={0};
    sol_t sol={{0}};
    int i,j,n=0,m,iobs;
    char msg[4096]="";

    arc_log(ARC_INFO,"arc_avepos: rcv=%d obs.n=%d\n",rcv,obs->n);

    for (i=0;i<3;i++) ra[i]=0.0;

    for (iobs=0;(m=arc_nextobsf(obs,&iobs,rcv))>0;iobs+=m) {

        for (i=j=0;i<m&&i<MAXOBS;i++) {
            data[j]=obs->data[iobs+i];
            if ((satsys(data[j].sat,NULL)&opt->navsys)&&
                opt->exsats[data[j].sat-1]!=1) j++;
        }
        if (j<=0||!screent(data[0].time,ts,ts,1.0)) continue; /* only 1 hz */

        if (!arc_pntpos(data,j,nav,opt,&sol,NULL,NULL,msg)) continue;

        for (i=0;i<3;i++) ra[i]+=sol.rr[i];
        n++;
    }
    if (n<=0) {
        arc_log(ARC_WARNING,"arc_avepos : no average of base station position\n");
        return 0;
    }
    for (i=0;i<3;i++) ra[i]/=n;
    return 1;
}
/* antenna phase center position ---------------------------------------------*/
static int arc_antpos(prcopt_t *opt, int rcvno, const obs_t *obs, const nav_t *nav,
                      const sta_t *sta, const char *posfile)
{
    double *rr=rcvno==1?opt->ru:opt->rb,del[3],pos[3],dr[3]={0};
    int i,postype=rcvno==1?opt->rovpos:opt->refpos;
    char *name;

    arc_log(ARC_INFO,"arc_antpos  : rcvno=%d\n",rcvno);

    if (postype==POSOPT_SINGLE) { /* average of single position */
        if (!arc_avepos(rr,rcvno,obs,nav,opt)) {
            arc_log(ARC_ERROR,"error : station pos computation");
            return 0;
        }
    }
    else if (postype==POSOPT_RINEX) { /* get from rinex header */
        if (arc_norm(stas[rcvno==1?0:1].pos,3)<=0.0) {
            arc_log(ARC_WARNING,"no position position in rinex header\n");
            return 0;
        }
        /* antenna delta */
        if (stas[rcvno==1?0:1].deltype==0) { /* enu */
            for (i=0;i<3;i++) del[i]=stas[rcvno==1?0:1].del[i];
            del[2]+=stas[rcvno==1?0:1].hgt;
            ecef2pos(stas[rcvno==1?0:1].pos,pos);
            enu2ecef(pos,del,dr);
        }
        else { /* xyz */
            for (i=0;i<3;i++) dr[i]=stas[rcvno==1?0:1].del[i];
        }
        for (i=0;i<3;i++) rr[i]=stas[rcvno==1?0:1].pos[i]+dr[i];
    }
    return 1;
}
/* open procssing session ----------------------------------------------------*/
static int arc_openses(const prcopt_t *popt, const solopt_t *sopt,
                       const filopt_t *fopt, nav_t *nav, pcvs_t *pcvs, pcvs_t *pcvr)
{
    int i;

    arc_log(ARC_INFO,"arc_openses :\n");

    /* satellite block id */
    arc_blockid(&nav->pcvs[0]);

    /* read satellite antenna parameters */
    arc_info(1,3,"read satellite antenna parameters");
    if (*fopt->satantp&&!(arc_readpcv(fopt->satantp, pcvs))) {
        arc_log(ARC_WARNING,"sat antenna pcv read error: %s\n",fopt->satantp);
        return 0;
    }
    /* read receiver antenna parameters */
    arc_info(2,3,"read receiver antenna parameters");
    if (*fopt->rcvantp&&!(arc_readpcv(fopt->rcvantp,pcvr))) {
        arc_log(ARC_WARNING,"rec antenna pcv read error: %s\n",fopt->rcvantp);
        arc_info(1,2,"rec antenna pcv read error");
        return 0;
    }
    /* use satellite L2 offset if L5 offset does not exists */
    for (i=0;i<pcvs->n;i++) {
        if (arc_norm(pcvs->pcv[i].off[2],3)>0.0) continue;
        arc_matcpy(pcvs->pcv[i].off[2],pcvs->pcv[i].off[1],3, 1);
        arc_matcpy(pcvs->pcv[i].var[2],pcvs->pcv[i].var[1],19,1);
    }
    for (i=0;i<pcvr->n;i++) {
        if (arc_norm(pcvr->pcv[i].off[2],3)>0.0) continue;
        arc_matcpy(pcvr->pcv[i].off[2],pcvr->pcv[i].off[1],3, 1);
        arc_matcpy(pcvr->pcv[i].var[2],pcvr->pcv[i].var[1],19,1);
    }
    arc_info(2,3,"read receiver antenna parameters is done");
    return 1;
}
/* close procssing session ---------------------------------------------------*/
static void arc_closeses(nav_t *nav, pcvs_t *pcvs, pcvs_t *pcvr)
{
    arc_log(ARC_INFO,"arc_closeses:\n");

    /* free antenna parameters */
    if (pcvs->pcv) free(pcvs->pcv); pcvs->pcv=NULL; pcvs->n=pcvs->nmax=0;
    if (pcvs->pcv) free(pcvr->pcv); pcvr->pcv=NULL; pcvr->n=pcvr->nmax=0;

    /* free erp data */
    if (nav->erp.data) free(nav->erp.data);
    nav->erp.data=NULL; nav->erp.n=nav->erp.nmax=0;

    /* close solution statistics and debug arc_log */
    arc_traceclose();
}
/* set antenna parameters ----------------------------------------------------*/
static void arc_setpcv(gtime_t time, prcopt_t *popt, nav_t *nav, const pcvs_t *pcvs,
                       const pcvs_t *pcvr, const sta_t *sta)
{
    pcv_t *pcv;
    double pos[3],del[3];
    int i,j,mode=PMODE_DGPS<=popt->mode&&popt->mode<=PMODE_FIXED;
    char id[64];

    /* set satellite antenna parameters */
    for (i=0;i<MAXSAT;i++) {
        if (!(satsys(i+1,NULL)&popt->navsys)) continue;
        if (!(pcv= arc_searchpcv(i+1,"",time,pcvs))) {
            satno2id(i+1,id);
            arc_log(ARC_WARNING,"no satellite antenna pcv: %s\n",id);
            continue;
        }
        nav->pcvs[i]=*pcv;
    }
    for (i=0;i<(mode?2:1);i++) {
        if (!strcmp(popt->anttype[i],"*")) { /* set by station parameters */
            strcpy(popt->anttype[i],sta[i].antdes);
            if (sta[i].deltype==1) { /* xyz */
                if (arc_norm(sta[i].pos,3)>0.0) {
                    ecef2pos(sta[i].pos,pos);
                    ecef2enu(pos,sta[i].del,del);
                    for (j=0;j<3;j++) popt->antdel[i][j]=del[j];
                }
            }
            else { /* enu */
                for (j=0;j<3;j++) popt->antdel[i][j]=stas[i].del[j];
            }
        }
        if (!(pcv= arc_searchpcv(0, popt->anttype[i], time, pcvr))) {
            arc_log(ARC_ERROR,"no receiver antenna pcv: %s\n",popt->anttype[i]);
            *popt->anttype[i]='\0';
            continue;
        }
        strcpy(popt->anttype[i],pcv->type);
        popt->pcvr[i]=*pcv;
    }
}
/* read ocean tide loading parameters ----------------------------------------*/
static void arc_readotl(prcopt_t *popt, const char *file, const sta_t *sta)
{
    int i,mode=PMODE_DGPS<=popt->mode&&popt->mode<=PMODE_FIXED;

    for (i=0;i<(mode?2:1);i++) {
        readblq(file,sta[i].name,popt->odisp[i]);
    }
}
/* output reference position -------------------------------------------------*/
static void outrpos(FILE *fp, const double *r, const solopt_t *opt)
{
    double pos[3],dms1[3],dms2[3];
    const char *sep=opt->sep;

    arc_log(ARC_INFO,"outrpos :\n");

    if (opt->posf==SOLF_LLH||opt->posf==SOLF_ENU) {
        ecef2pos(r,pos);
        if (opt->degf) {
            deg2dms(pos[0]*R2D,dms1,5);
            deg2dms(pos[1]*R2D,dms2,5);
            fprintf(fp,"%3.0f%s%02.0f%s%08.5f%s%4.0f%s%02.0f%s%08.5f%s%10.4f",
                    dms1[0],sep,dms1[1],sep,dms1[2],sep,dms2[0],sep,dms2[1],
                    sep,dms2[2],sep,pos[2]);
        }
        else {
            fprintf(fp,"%13.9f%s%14.9f%s%10.4f",pos[0]*R2D,sep,pos[1]*R2D,
                    sep,pos[2]);
        }
    }
    else if (opt->posf==SOLF_XYZ) {
        fprintf(fp,"%14.4f%s%14.4f%s%14.4f",r[0],sep,r[1],sep,r[2]);
    }
}
/* output header -------------------------------------------------------------*/
static void outheader(FILE *fp, char **file, int n, const prcopt_t *popt,
                      const solopt_t *sopt)
{
    const char *s1[]={"GPST","UTC","JST"};
    gtime_t ts,te;
    double t1,t2;
    int i,j,w1,w2;
    char s2[32],s3[32];

    arc_log(ARC_INFO,"outheader: n=%d\n",n);

    if (sopt->outhead) {
        if (!*sopt->prog) {
            fprintf(fp,"%s program   : ARC-SRTK ver.%s\n",COMMENTH,"1.0");
        }
        else {
            fprintf(fp,"%s program   : %s\n",COMMENTH,sopt->prog);
        }
        for (i=0;i<n;i++) {
            fprintf(fp,"%s inp file  : %s\n",COMMENTH,file[i]);
        }
        for (i=0;i<obss.n;i++)    if (obss.data[i].rcv==1) break;
        for (j=obss.n-1;j>=0;j--) if (obss.data[j].rcv==1) break;
        if (j<i) {fprintf(fp,"\n%s no rover obs data\n",COMMENTH); return;}
        ts=obss.data[i].time;
        te=obss.data[j].time;
        t1=time2gpst(ts,&w1);
        t2=time2gpst(te,&w2);
        if (sopt->times>=1) ts=gpst2utc(ts);
        if (sopt->times>=1) te=gpst2utc(te);
        if (sopt->times==2) ts=timeadd(ts,9*3600.0);
        if (sopt->times==2) te=timeadd(te,9*3600.0);
        time2str(ts,s2,1);
        time2str(te,s3,1);
        fprintf(fp,"%s obs start : %s %s (week%04d %8.1fs)\n",COMMENTH,s2,s1[sopt->times],w1,t1);
        fprintf(fp,"%s obs end   : %s %s (week%04d %8.1fs)\n",COMMENTH,s3,s1[sopt->times],w2,t2);
    }
    if (sopt->outopt) {
        outprcopt(fp,popt);
    }
    if (PMODE_DGPS<=popt->mode&&popt->mode<=PMODE_FIXED&&popt->mode!=PMODE_MOVEB) {
        fprintf(fp,"%s ref pos   :",COMMENTH);
        outrpos(fp,popt->rb,sopt);
        fprintf(fp,"\n");
    }
    if (sopt->outhead||sopt->outopt) fprintf(fp,"%s\n",COMMENTH);

    outsolhead(fp,sopt);
}
/* write header to output file -----------------------------------------------*/
static int outhead(const char *outfile, char **infile, int n,
                   const prcopt_t *popt, const solopt_t *sopt)
{
    FILE *fp=stdout;

    arc_log(ARC_INFO,"outhead: outfile=%s n=%d\n",outfile,n);

    if (*outfile) {
        createdir(outfile);

        if (!(fp=fopen(outfile,"w"))) {
            arc_log(ARC_ERROR,"error : open output file %s",outfile);
            return 0;
        }
    }
    /* output header */
    outheader(fp,infile,n,popt,sopt);

    if (*outfile) fclose(fp);

    return 1;
}
/* open output file for append -----------------------------------------------*/
static FILE *openfile(const char *outfile)
{
    arc_log(ARC_INFO,"openfile: outfile=%s\n",outfile);

    return !*outfile?stderr:fopen(outfile,"a");
}
/* execute processing session ------------------------------------------------*/
static int arc_execses(gtime_t ts, gtime_t te, double ti, const prcopt_t *popt,
                       const solopt_t *sopt, const filopt_t *fopt, int flag,
                       char **infile, const int *index, int n, char *outfile)
{
    FILE *fp=NULL;
    prcopt_t popt_=*popt;
    char path[1024],statfile[1024];

    arc_log(ARC_INFO,"arc_execses : n=%d outfile=%s\n",n,outfile);

    /* read erp data */
    arc_log(ARC_INFO,"read erp data : %s \n",fopt->eop);
    if (*fopt->eop) {
        free(navs.erp.data); navs.erp.data=NULL; navs.erp.n=navs.erp.nmax=0;
        reppath(fopt->eop,path,ts,"","");
        arc_info(3,3,"read erp data");
        if (!readerp(path,&navs.erp)) {
            arc_log(ARC_WARNING,"no erp data %s\n",path);
        }
    }
    /* read obs and nav data */
    arc_log(ARC_INFO,"read obs and nav data \n");
    if (!arc_readobsnav(ts,te,ti,infile,index,n,&popt_,&obss,&navs,stas)) return 0;

    /* read dcb parameters */
    arc_log(ARC_INFO,"read dcb parameters : %s \n",fopt->dcb);
    if (*fopt->dcb) {
        arc_info(10,3,"read dcb parameters");
        reppath(fopt->dcb,path,ts,"","");
        arc_readdcb(path, &navs, stas);
    }
    /* set antenna paramters */
    arc_log(ARC_INFO,"set antenna paramters \n");
    if (popt_.mode!=PMODE_SINGLE) {
        arc_info(10,3,"set antenna paramters");
        arc_setpcv(obss.n>0?obss.data[0].time:timeget(),&popt_,
                   &navs,&pcvss,&pcvsr,stas);
    }
    /* read ocean tide loading parameters */
    arc_log(ARC_INFO,"read ocean tide loading parameters \n");
    if (popt_.mode>PMODE_SINGLE&&*fopt->blq) {
        arc_info(10,3,"read ocean tide loading parameters");
        arc_readotl(&popt_,fopt->blq,stas);
    }
    if (PMODE_DGPS<=popt_.mode&&popt_.mode<=PMODE_STATIC) {
        if (!arc_antpos(&popt_,2,&obss,&navs,stas,fopt->stapos)) {
            arc_freeobsnav(&obss,&navs);
            return 0;
        }
    }
    /* open solution statistics */
    if (flag&&sopt->sstat>0) {
        strcpy(statfile,outfile);
        strcat(statfile,".stat");
#if USERTKLIB
        rtkclosestat2();
        rtkopenstat2(statfile,sopt->sstat);
#else
        rtkclosestat();
        rtkopenstat(statfile,sopt->sstat);
#endif
    }
    /* write header to output file */
    if (flag&&!outhead(outfile,infile,n,&popt_,sopt)) {
        arc_freeobsnav(&obss,&navs);
        fprintf(stderr,"Error: no setting output file ");
        getchar();
        return 0;
    }
    iobsu=iobsr=isbs=revs=aborts=0;

    if (popt_.mode==PMODE_SINGLE||popt_.soltype==0) {
        if ((fp=openfile(outfile))) {
            arc_procpos(fp,&popt_,sopt,0); /* forward */
            fclose(fp);
        }
    }
    else if (popt_.soltype==1) {
        if ((fp=openfile(outfile))) {
            revs=1; iobsu=iobsr=obss.n-1;
            arc_procpos(fp,&popt_,sopt,0); /* backward */
            fclose(fp);
        }
    }
    else { /* combined */
        solf=(sol_t *)malloc(sizeof(sol_t)*nepoch);
        solb=(sol_t *)malloc(sizeof(sol_t)*nepoch);
        rbf=(double *)malloc(sizeof(double)*nepoch*3);
        rbb=(double *)malloc(sizeof(double)*nepoch*3);

        if (solf&&solb) {
            isolf=isolb=0;
            arc_procpos(NULL,&popt_,sopt,1); /* forward */
            revs=1; iobsu=iobsr=obss.n-1;
            arc_procpos(NULL,&popt_,sopt,1); /* backward */

            /* combine forward/backward solutions */
            if (!aborts&&(fp=openfile(outfile))) {
                arc_combres(fp,&popt_,sopt);
                fclose(fp);
            }
        }
        else arc_log(ARC_ERROR,"error : memory allocation \n");
        free(solf);
        free(solb);
        free(rbf);
        free(rbb);
    }
    /* free obs and nav data */
    arc_freeobsnav(&obss,&navs);
    return aborts?1:0;
}
/* execute processing session for each rover ---------------------------------*/
static int arc_execses_r(gtime_t ts, gtime_t te, double ti, const prcopt_t *popt,
                         const solopt_t *sopt, const filopt_t *fopt, int flag,
                         char **infile, const int *index, int n, char *outfile,
                         const char *rov)
{
    int stat=0;

    arc_log(ARC_INFO, "execses_r: n=%d outfile=%s\n", n, outfile);

    /* execute processing session */
    stat=arc_execses(ts,te,ti,popt,sopt,fopt,flag,infile,index,n,outfile);
    return stat;
}
/* execute processing session for each base station --------------------------*/
static int arc_execses_b(gtime_t ts, gtime_t te, double ti, const prcopt_t *popt,
                         const solopt_t *sopt, const filopt_t *fopt, int flag,
                         char **infile, const int *index, int n, char *outfile,
                         const char *rov, const char *base)
{
    int stat=0;

    arc_log(ARC_INFO, "execses_b: n=%d outfile=%s\n", n, outfile);

    /* read prec ephemeris and sbas data */
    arc_readpreceph(infile,n,popt,&navs);

    /* execute processing session */
    stat=arc_execses_r(ts,te,ti,popt,sopt,fopt,flag,infile,index,n,outfile,rov);

    /* free prec ephemeris and sbas data */
    arc_freepreceph(&navs);
    return stat;
}
/* post-processing positioning ------------------------------------------------*/
extern int arc_postpos(gtime_t ts, gtime_t te, double ti, double tu,
                       prcopt_t *popt, const solopt_t *sopt,
                       const filopt_t *fopt, char **infile, int n, char *outfile,
                       const char *rov, const char *base)
{
    int i,stat=0,index[MAXINFILE]={0};

    arc_log(ARC_INFO,"arc_postpos : ti=%.0f tu=%.0f n=%d outfile=%s\n",ti,tu,n,outfile);

    /* exclude bds geo satellite */
    if (popt->exclude_bds_geo) {
        arc_log(ARC_INFO,"arc_postpos : exclude bds geo satellite");
        arc_info(1,4,"exclude bds geo satellite");
        arc_exclude_bds_geo(popt);
    }
    /* open processing session */
    if (!arc_openses(popt,sopt,fopt,&navs,&pcvss,&pcvsr)) return -1;

    for (i=0;i<n;i++) index[i]=i;

    /* execute processing session */
    stat=arc_execses_b(ts,te,ti,popt,sopt,fopt,1,infile,index,n,outfile,rov,base);

    /* close processing session */
    arc_closeses(&navs,&pcvss,&pcvsr);
    return stat;
}
