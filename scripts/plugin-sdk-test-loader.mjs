import { pathToFileURL } from "node:url";

export async function resolve(specifier, context, nextResolve) {
  if (specifier === "@ghosterm/plugin-sdk") {
    const target = process.env.GHOSTERM_TEST_PLUGIN_SDK_ENTRY;
    if (!target) throw new Error("GHOSTERM_TEST_PLUGIN_SDK_ENTRY is required");
    return { url: pathToFileURL(target).href, shortCircuit: true };
  }
  return nextResolve(specifier, context);
}
