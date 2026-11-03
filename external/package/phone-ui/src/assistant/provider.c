/*
 * Assistant's providers: their APIs, models and keys (assistant.h).
 */

#include <string.h>

#include "assistant.h"

#define MODEL_DEFAULT "deepseek-flash"

const struct provider ai_providers[] = {
	{ "DeepSeek", "https://api.deepseek.com/chat/completions", "assistant.deepseek.key",
	  "Create one on platform.deepseek.com, API keys.",
	  { { "deepseek-flash", "Fast, cheap" },
	    { "deepseek-v4-pro", "Smarter, slower" } },
	  true, "https://api.deepseek.com/v1/tools/search" },
};
const unsigned ai_provider_count = sizeof(ai_providers) / sizeof(ai_providers[0]);
_Static_assert(sizeof(ai_providers) / sizeof(ai_providers[0]) <= MAX_PROVIDERS, "MAX_PROVIDERS");

static const struct model *model_find(const char *id, const struct provider **provider)
{
	for (unsigned p = 0; p < ai_provider_count; p++)
		for (const struct model *m = ai_providers[p].models; m->id; m++)
			if (!strcmp(m->id, id)) {
				if (provider)
					*provider = &ai_providers[p];
				return m;
			}
	return NULL;
}

/* The model chosen in the settings, and its provider */
const struct model *ai_model_current(const struct provider **provider)
{
	const struct model *m = model_find(ui_setting_get("assistant.model", MODEL_DEFAULT), provider);
	return m ? m : model_find(MODEL_DEFAULT, provider);
}

const char *ai_provider_key(const struct provider *p)
{
	return ui_setting_get(p->key_setting, "");
}

bool ai_search_on(const struct provider *p)
{
	return p->search_url && ui_setting_get_int("assistant.search", 0);
}
