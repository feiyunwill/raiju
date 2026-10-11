#include <stdbool.h>
#include <stdio.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

int sceNotificationSend(int userId, bool isLogged, const char *payload);

/** Toast body: the payload's first proof of life, naming the version it was built from. */
static const char toast_tmpl[] =
	"{\n"
	"  \"rawData\": {\n"
	"    \"viewTemplateType\": \"InteractiveToastTemplateB\",\n"
	"    \"channelType\": \"Downloads\",\n"
	"    \"useCaseId\": \"IDC\",\n"
	"    \"toastOverwriteType\": \"No\",\n"
	"    \"isImmediate\": true,\n"
	"    \"priority\": 100,\n"
	"    \"viewData\": {\n"
	"      \"icon\": {\n"
	"        \"type\": \"Predefined\",\n"
	"        \"parameters\": {\n"
	"          \"icon\": \"download\"\n"
	"        }\n"
	"      },\n"
	"      \"message\": {\n"
	"        \"body\": \"Raiju %s\"\n"
	"      },\n"
	"      \"subMessage\": {\n"
	"        \"body\": \"payload running\"\n"
	"      }\n"
	"    },\n"
	"    \"platformViews\": {\n"
	"      \"previewDisabled\": {\n"
	"        \"viewData\": {\n"
	"          \"icon\": {\n"
	"            \"type\": \"Predefined\",\n"
	"            \"parameters\": {\n"
	"              \"icon\": \"download\"\n"
	"            }\n"
	"          },\n"
	"          \"message\": {\n"
	"            \"body\": \"Raiju %s is running\"\n"
	"          }\n"
	"        }\n"
	"      }\n"
	"    }\n"
	"  },\n"
	"  \"localNotificationId\": \"1\"\n"
	"}";

int main(void) {
	char payload[2048];
	int notified;

	if (snprintf(payload, sizeof(payload), toast_tmpl, RAIJU_VERSION,
		     RAIJU_VERSION) < 0) {
		return 1;
	}

	printf("Raiju %s\n", RAIJU_VERSION);
	fflush(stdout);

	notified = sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
				       payload);
	printf("notification result %d\n", notified);
	fflush(stdout);

	return notified;
}
