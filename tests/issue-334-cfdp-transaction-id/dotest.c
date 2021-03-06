/* Test for issue 334-cfdp-transaction-id
 * Ensure that CFDP actually returns a transaction
 * ID to the caller of cfdp_put, cfdp_get,
 * and cfdp_rput.
 *
 * Author: Samuel Jero
 * Date: December 2011			*/

#include <cfdp.h>
#include <stdlib.h>
#include "check.h"
#include "testutil.h"

typedef struct
{
	CfdpHandler		faultHandlers[16];
	CfdpNumber		destinationEntityNbr;
	char			sourceFileNameBuf[256];
	char			*sourceFileName;
	char			destFileNameBuf[256];
	char			*destFileName;
	BpUtParms		utParms;
	MetadataList		msgsToUser;
	MetadataList		fsRequests;
	CfdpTransactionId	transactionId;
} CfdpReqParms;


int main(int argc, char **argv)
{
	CfdpReqParms parms;
	uvast src;
	uvast tid;
	int ret;

	/* Start ION */
	printf("Starting ION...\n");
	_xadmin("ionadmin", "", "cfdp.ipn.bp.ltp.udp/config.ionrc");
	_xadmin("ltpadmin", "", "cfdp.ipn.bp.ltp.udp/config.ltprc");
	_xadmin("bpadmin", "", "cfdp.ipn.bp.ltp.udp/config.bprc");
	_xadmin("ipnadmin", "", "cfdp.ipn.bp.ltp.udp/config.ipnrc");
	_xadmin("cfdpadmin", "", "cfdp.ipn.bp.ltp.udp/config.cfdprc");

	/* Attach to CFDP */
	fail_unless(cfdp_attach() >= 0);

	/*Send test file*/
	memset((char *) &parms, 0, sizeof(CfdpReqParms));
	cfdp_compress_number(&parms.destinationEntityNbr, 1);
	parms.utParms.lifespan = 86400;
	parms.utParms.classOfService = BP_STD_PRIORITY;
	parms.utParms.custodySwitch = NoCustodyRequested;
	parms.fsRequests=0;
	parms.msgsToUser=0;
	strcpy(parms.sourceFileNameBuf, "dotest.c");
	strcpy(parms.destFileNameBuf, "testfile");
	parms.sourceFileName=parms.sourceFileNameBuf;
	parms.destFileName=parms.destFileNameBuf;

	printf("Starting CFDP Transaction...\n");
	fail_unless(cfdp_put(&(parms.destinationEntityNbr),
						sizeof(BpUtParms),
						(unsigned char *) &(parms.utParms),
						parms.sourceFileName,
						parms.destFileName, NULL,
						parms.faultHandlers, 0, NULL,
						parms.msgsToUser,
						parms.fsRequests,
						&(parms.transactionId)) >= 0);
	printf("Call returned...\n");

	/* Stop ION */
	printf("Stopping ION...\n");
	writeErrmsgMemos();
	_xadmin("cfdpadmin", "", ".");
	ionstop();

	/*Fail on ION start/stop errors*/
	if(check_summary(argv[0])==1){
		return 1;
	}

	/* Check to see if a transaction ID was returned*/
	printf("Checking transaction ID...\n");
	cfdp_decompress_number(&src, &parms.transactionId.sourceEntityNbr);
	cfdp_decompress_number(&tid, &parms.transactionId.transactionNbr);
	if(src != 0 && tid!=0) {
		/*Success!*/
		ret=0;
		printf("A valid Transaction ID was returned. SUCCESS!\n");
	} else {
		ret=1;/*Failure*/
		printf("No Transaction ID returned. FAILURE!\n");
	}

	return ret;
}
