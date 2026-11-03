package com.hanxi.ghosterm

import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

@Composable
internal fun GhostermIcon(icon: UiIcon, description: String?=null, modifier: Modifier=Modifier,
    tint: Color=LocalContentColor.current) {
    Icon(painterResource(icon.resource),contentDescription=description,
        modifier=modifier.size(GhostermDimensions.icon),tint=tint)
}

@Composable
internal fun GhostermIconButton(icon: UiIcon, label: String, onClick: () -> Unit,
    modifier: Modifier=Modifier, enabled: Boolean=true, selected: Boolean=false) {
    IconButton(onClick=onClick,enabled=enabled,modifier=modifier.sizeIn(minWidth=GhostermDimensions.touch,minHeight=GhostermDimensions.touch)
        .background(if(selected) MaterialTheme.colorScheme.primaryContainer else Color.Transparent,MaterialTheme.shapes.small)) {
        GhostermIcon(icon,label,tint=if(enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurface.copy(alpha=.38f))
    }
}

@Composable
internal fun GhostermGlyph(icon: UiIcon, modifier: Modifier=Modifier) {
    Box(modifier.size(34.dp).background(MaterialTheme.colorScheme.surfaceContainerHigh,MaterialTheme.shapes.small),
        contentAlignment=Alignment.Center) { GhostermIcon(icon,tint=MaterialTheme.colorScheme.primary) }
}

@Composable
internal fun GhostermGroup(modifier: Modifier=Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(modifier.fillMaxWidth().clip(MaterialTheme.shapes.medium).background(MaterialTheme.colorScheme.surface),content=content)
}

@Composable
internal fun GhostermSectionLabel(label: String, trailing: String?=null) {
    Row(Modifier.fillMaxWidth().padding(horizontal=4.dp,vertical=10.dp),verticalAlignment=Alignment.CenterVertically) {
        Text(label,Modifier.weight(1f),style=MaterialTheme.typography.labelMedium,color=MaterialTheme.colorScheme.primary)
        trailing?.let { Text(it,style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.onSurfaceVariant) }
    }
}

/** Visual row only. Callers own state, navigation and any asynchronous operations. */
@Composable
internal fun GhostermActionRow(title: String, subtitle: String?=null, icon: UiIcon?=null,
    modifier: Modifier=Modifier, enabled: Boolean=true, onClick: () -> Unit) {
    Row(modifier.fillMaxWidth().heightIn(min=GhostermDimensions.row).clickable(enabled=enabled,role=Role.Button,onClick=onClick)
        .padding(horizontal=14.dp,vertical=12.dp),verticalAlignment=Alignment.CenterVertically,
        horizontalArrangement=Arrangement.spacedBy(12.dp)) {
        icon?.let { GhostermGlyph(it) }
        Column(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(4.dp)) {
            Text(title,style=MaterialTheme.typography.titleSmall,
                color=MaterialTheme.colorScheme.onSurface.copy(alpha=if(enabled) 1f else .38f))
            subtitle?.let { Text(it,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant) }
        }
        GhostermIcon(UiIcon.Arrow,tint=MaterialTheme.colorScheme.onSurfaceVariant,modifier=Modifier.size(17.dp))
    }
}

@Composable
internal fun GhostermDivider() { HorizontalDivider(color=MaterialTheme.colorScheme.outline) }

@Composable
internal fun GhostermSwitch(checked: Boolean, enabled: Boolean) {
    Switch(checked,onCheckedChange=null,enabled=enabled,colors=SwitchDefaults.colors(
        checkedTrackColor=MaterialTheme.colorScheme.primary,checkedBorderColor=MaterialTheme.colorScheme.primary,
        checkedThumbColor=MaterialTheme.colorScheme.onPrimary,
        uncheckedTrackColor=MaterialTheme.colorScheme.surfaceContainerHigh,
        uncheckedBorderColor=MaterialTheme.colorScheme.onSurfaceVariant,
        uncheckedThumbColor=MaterialTheme.colorScheme.onSurfaceVariant))
}

/** Decorative grip; the sheet itself owns drag, dismiss and accessibility actions. */
@Composable
internal fun GhostermSheetHandle() {
    Box(Modifier.fillMaxWidth().height(36.dp),contentAlignment=Alignment.Center) {
        Box(Modifier.size(32.dp,4.dp).background(MaterialTheme.colorScheme.onSurfaceVariant,MaterialTheme.shapes.extraSmall))
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun GhostermTopBar(title: @Composable () -> Unit, navigationIcon: @Composable () -> Unit = {},
    actions: @Composable RowScope.() -> Unit = {}) {
    TopAppBar(title=title,navigationIcon=navigationIcon,actions=actions,expandedHeight=60.dp,
        colors=TopAppBarDefaults.topAppBarColors(containerColor=MaterialTheme.colorScheme.background,
            scrolledContainerColor=MaterialTheme.colorScheme.background))
}

@Composable
internal fun GhostermSearchField(value: String, onValueChange: (String) -> Unit, modifier: Modifier=Modifier,
    placeholder:String="搜索名称或地址") {
    BasicTextField(value,onValueChange,singleLine=true,textStyle=MaterialTheme.typography.bodyMedium.copy(color=MaterialTheme.colorScheme.onSurface),
        cursorBrush=SolidColor(MaterialTheme.colorScheme.primary),
        modifier=modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surfaceContainerHigh,RoundedCornerShape(28.dp))
            .semantics { contentDescription=placeholder },decorationBox={ inner ->
            Row(Modifier.fillMaxWidth().heightIn(min=50.dp).padding(horizontal=16.dp),verticalAlignment=Alignment.CenterVertically,
                horizontalArrangement=Arrangement.spacedBy(10.dp)) {
                GhostermIcon(UiIcon.Search,tint=MaterialTheme.colorScheme.onSurfaceVariant)
                Box(Modifier.weight(1f).padding(vertical=10.dp)) {
                    if(value.isEmpty()) Text(placeholder,style=MaterialTheme.typography.bodyMedium,color=MaterialTheme.colorScheme.onSurfaceVariant)
                    inner()
                }
            }
        })
}
