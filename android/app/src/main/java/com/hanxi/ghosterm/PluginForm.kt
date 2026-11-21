package com.hanxi.ghosterm

internal data class PluginForm(val id: String, val title: String, val fields: List<Field>) {
    data class Field(val id: String, val label: String, val kind: Kind)
    data class Option(val id: String, val label: String)
    sealed interface Kind {
        data class Text(val required: Boolean, val maxLength: Int) : Kind
        data object Toggle : Kind
        data class Choice(val options: List<Option>) : Kind
    }
    sealed interface Answer {
        data object Dismissed : Answer
        data class Submitted(val values: Map<String, PluginViewValue.Value>) : Answer
    }

    fun outcome(answer: Answer): ByteArray = with(PluginViewValue) {
        val value = linkedMapOf<String, PluginViewValue.Value>("requestId" to PluginViewValue.Value.Text(id))
        when (answer) {
            Answer.Dismissed -> value["kind"] = PluginViewValue.Value.Text("dismissed")
            is Answer.Submitted -> {
                require(answer.values.size == fields.size)
                val supplied = fields.associate { field ->
                    val input = checkNotNull(answer.values[field.id])
                    when (val kind = field.kind) {
                        is Kind.Text -> checkedText(input, kind.maxLength, kind.required)
                        Kind.Toggle -> require(input is PluginViewValue.Value.Bool)
                        is Kind.Choice -> require(kind.options.any { PluginViewValue.Value.Text(it.id) == input })
                    }
                    field.id to input
                }
                value["kind"] = PluginViewValue.Value.Text("submitted")
                value["values"] = PluginViewValue.Value.Fields(supplied)
            }
        }
        val body = compact(PluginViewValue.Value.Fields(value))
        require(utf8(body).size <= BODY_LIMIT)
        utf8("""{"kind":"success","value":$body}""")
    }

    companion object {
        const val method = "view.form.request"
        private const val BODY_LIMIT = 2048
        private val identifier = Regex("[A-Za-z][A-Za-z0-9_-]{0,31}")

        fun decode(bytes: ByteArray): PluginForm = with(PluginViewValue) {
            val form = envelope(bytes)
            form.keys(setOf("requestId", "title", "fields"))
            require(utf8(compact(PluginViewValue.Value.Fields(form))).size <= BODY_LIMIT)
            val fields = form["fields"].items().also { require(it.size in 1..8) }.map { decodeField(it.fields()) }
            require(fields.map { it.id }.distinct().size == fields.size)
            PluginForm(form["requestId"].revision(), checkedText(form["title"], 96, true), captured(fields))
        }

        private fun decodeField(field: Map<String, PluginViewValue.Value>): Field = with(PluginViewValue) {
            val common = setOf("kind", "id", "label")
            val kind = when (field["kind"].text(8)) {
                "text" -> {
                    field.keys(common + setOf("required", "maxLength"))
                    val required = (field["required"] as? PluginViewValue.Value.Bool)?.value ?: error("表单必填标志无效")
                    Kind.Text(required, field["maxLength"].integer(1..256))
                }
                "toggle" -> { field.keys(common); Kind.Toggle }
                "choice" -> {
                    field.keys(common + "options")
                    val options = field["options"].items().also { require(it.size in 1..8) }.map {
                        val option = it.fields()
                        option.keys(setOf("id", "label"))
                        Option(key(option["id"]), checkedText(option["label"], 96, true))
                    }
                    require(options.map { it.id }.distinct().size == options.size)
                    Kind.Choice(captured(options))
                }
                else -> error("未知的表单字段类型")
            }
            Field(key(field["id"]), checkedText(field["label"], 96, true), kind)
        }

        private fun key(value: PluginViewValue.Value?): String = with(PluginViewValue) {
            value.text(32).also { require(identifier.matches(it) && it !in setOf("constructor", "prototype")) }
        }
        private fun checkedText(value: PluginViewValue.Value?, maximum: Int, required: Boolean): String = with(PluginViewValue) {
            value.text(maximum).also { text ->
                require(text.none { it.code < 32 || it.code == 127 } && (!required || text.isNotBlank()))
            }
        }
    }
}
